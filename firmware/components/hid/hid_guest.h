/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes opt-in bounded three-link NimBLE startup for hardware bring-up. Firmware
 * main remains disarmed; callers must verify board recovery and explicitly
 * start this service before it can advertise or accept a guest. It also
 * exposes bounded NimBLE host-loop pairing requests and short status events.
 * Host/USB routes through one selected channel at a time. A bounded host-loop
 * snapshot exposes each authenticated live peer's opaque token and readiness;
 * retained inventory uses the same serialized bridge. */
#ifndef ESP32_KVM_HID_GUEST_H
#define ESP32_KVM_HID_GUEST_H

#include "esp_err.h"
#include "hid_pairing.h"
#include "hid_gatt.h"
#include "hid_report.h"

/** Authenticated, opaque state for one one-based HID connection slot. */
typedef struct {
    uint8_t token[HID_PAIRING_TOKEN_LEN];
    bool ready;
    bool subscribed;
} hid_guest_slot_snapshot_t;

/** Pairing event types delivered to a future host status transport. */
typedef enum {
    HID_GUEST_PAIRING_OPENED,
    HID_GUEST_PAIRING_CHALLENGE,
    HID_GUEST_PAIRING_CLOSED,
    HID_GUEST_BONDED,
    HID_GUEST_PAIRING_REJECTED,
    HID_GUEST_PAIRING_CAPACITY,
    HID_GUEST_PAIRING_TIMEOUT,
    /** Signals the USB worker to clear its connected-token STATUS cache. */
    HID_GUEST_DISCONNECTED
} hid_guest_pairing_event_type_t;

/** Status event; number is present only for CHALLENGE, token only for BONDED. */
typedef struct {
    hid_guest_pairing_event_type_t type;
    uint16_t connection_handle;
    uint32_t challenge_id;
    uint32_t number;
    uint64_t deadline_ms;
    hid_token_t token;
} hid_guest_pairing_event_t;

/** Receives short status events on the NimBLE host thread. */
typedef void (*hid_guest_pairing_event_fn)(const hid_guest_pairing_event_t *event, void *context);

/** Initializes NVS/NimBLE, registers HID, and starts the BLE host task. */
esp_err_t hid_guest_start(void);
/** Sets a nonblocking status sink before the BLE service starts. */
void hid_guest_pairing_set_events(hid_guest_pairing_event_fn callback, void *context);
/** Opens pairing for 60 seconds; caller must run on the NimBLE host thread. */
esp_err_t hid_guest_pairing_open(void);
/** Cancels pairing; caller must run on the NimBLE host thread. */
void hid_guest_pairing_cancel(void);
/** Confirms or rejects the pending numeric comparison on the host thread. */
esp_err_t hid_guest_pairing_confirm(uint32_t challenge_id, bool approved);
/** Copies current pairing state for a host-thread status bridge. */
void hid_guest_pairing_snapshot(hid_pairing_t *output);
/** Looks up the current authenticated connection's opaque token on the NimBLE host loop.
 * Clears output and returns false for unbound, disconnected, or unauthenticated peers. */
bool hid_guest_current_bond_token(hid_token_t *output);
/** Copies all authenticated live slots on the NimBLE host thread. */
bool hid_guest_snapshot_slots(hid_guest_slot_snapshot_t output[HID_GATT_MAX_CONNECTIONS]);
/** Copies all live slots through one bounded host-loop request; zeros on failure. */
bool hid_guest_request_slots(hid_guest_slot_snapshot_t output[HID_GATT_MAX_CONNECTIONS]);
/** Copies retained opaque tokens only; call on the NimBLE host thread. */
bool hid_guest_retained_bonds(hid_token_t output[HID_PAIRING_MAX_BONDS], size_t *count);
/** Removes one bond only after explicit confirmation on the host thread. */
esp_err_t hid_guest_pairing_forget(hid_token_t token, bool confirmed);
/** Disarms and terminates the current guest after uncertain all-up delivery.
 * Call on the NimBLE host thread; a disconnected channel succeeds. */
esp_err_t hid_guest_disconnect_current(void);
/** Disarms and terminates one channel on the NimBLE host thread. */
esp_err_t hid_guest_disconnect_slot(uint8_t slot);
/** Queues a coalesced disconnect on NimBLE's host event loop from another task.
 * Returns success when queued; it does not await the radio disconnect. */
esp_err_t hid_guest_request_disconnect(void);
/** Reads readiness on the NimBLE host loop within a bounded wait. */
bool hid_guest_request_ready(void);
/** Reads readiness for one bounded, one-based GATT channel. */
bool hid_guest_request_ready_slot(uint8_t slot);
/** Arms after an all-up baseline on the NimBLE host loop. */
bool hid_guest_request_arm(void);
/** Arms one selected channel after its all-up baseline. */
bool hid_guest_request_arm_slot(uint8_t slot);
/** Sends all-up reports and disarms on the NimBLE host loop. */
bool hid_guest_request_release(void);
/** Releases and disarms one selected channel. */
bool hid_guest_request_release_slot(uint8_t slot);
/** Sends one keyboard state through the bounded host-loop bridge. */
bool hid_guest_request_keyboard(const uint8_t keys[HID_KEYBOARD_REPORT_LEN]);
/** Sends keyboard state to one selected channel. */
bool hid_guest_request_keyboard_slot(uint8_t slot, const uint8_t keys[HID_KEYBOARD_REPORT_LEN]);
/** Sends relative mouse state through the bounded host-loop bridge. */
bool hid_guest_request_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                             int8_t wheel, int8_t pan);
/** Sends pointer state to one selected channel. */
bool hid_guest_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy,
                                  int8_t wheel, int8_t pan);
/** Sends one consumer usage through the bounded host-loop bridge. */
bool hid_guest_request_consumer(uint16_t usage);
/** Sends consumer usage to one selected channel. */
bool hid_guest_request_consumer_slot(uint8_t slot, uint16_t usage);
/** Queues a fail-local disconnect of one selected channel. */
esp_err_t hid_guest_request_disconnect_slot(uint8_t slot);
/** Opens a pairing window on the NimBLE host loop within a bounded wait. */
bool hid_guest_request_pair_begin(void);
/** Cancels pairing on the NimBLE host loop within a bounded wait. */
bool hid_guest_request_pair_cancel(void);
/** Answers the exact challenge on the NimBLE host loop within a bounded wait. */
bool hid_guest_request_pair_reply(uint32_t challenge_id, bool approved);
/** Retrieves only a connected authenticated peer's opaque token through a bounded RPC.
 * Always clears output on failure; never returns BLE addresses or key material. */
bool hid_guest_request_current_bond_token(uint8_t output[HID_PAIRING_TOKEN_LEN]);
/** Copies up to eight retained tokens within a bounded NimBLE host-loop request.
 * Clears output and count on failure; an empty successful inventory is authoritative. */
bool hid_guest_request_retained_bonds(uint8_t output[HID_PAIRING_MAX_BONDS][HID_PAIRING_TOKEN_LEN],
                                      uint8_t *count);
/** Forgets one confirmed opaque bond token on the NimBLE host loop. */
bool hid_guest_request_forget_bond(const uint8_t token[HID_PAIRING_TOKEN_LEN]);

#endif
