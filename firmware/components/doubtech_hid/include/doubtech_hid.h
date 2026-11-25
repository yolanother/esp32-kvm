/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Public API of the doubtech_hid component: opt-in bounded three-link NimBLE
 * HID startup for hardware bring-up. Callers must verify board recovery and
 * explicitly start the service before it can advertise or accept a guest. It
 * also exposes bounded NimBLE host-loop pairing requests and short status
 * events. An application can route input to one selected slot at a time. A bounded
 * host-loop snapshot exposes each authenticated live peer's opaque token and
 * readiness; retained inventory uses the same serialized bridge. */
#ifndef DOUBTECH_HID_H
#define DOUBTECH_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Source-side channel bound; wire-visible live capacity remains one pending proof. */
#define DOUBTECH_HID_MAX_CONNECTIONS 3
/** Maximum retained bond tokens. */
#define DOUBTECH_HID_MAX_BONDS 8
/** Exactly 16 opaque bytes; the application chooses their meaning. */
#define DOUBTECH_HID_TOKEN_LEN 16
/** Length of one keyboard input report; must equal the internal HID_KEYBOARD_REPORT_LEN. */
#define DOUBTECH_HID_KEYBOARD_REPORT_LEN 8
/** Longest advertised and GAP device name in bytes (not characters): the 31-byte
 * advertising payload less flags (3), HID UUID16 (4), appearance (4) and the
 * name header (2). */
#define DOUBTECH_HID_NAME_MAX_BYTES 18
/** Longest Device Information manufacturer or model string in bytes. */
#define DOUBTECH_HID_DIS_STRING_MAX_BYTES 32

struct hid_pairing;

/** An opaque application bond token; never a BLE address or key material. */
typedef struct { uint8_t bytes[DOUBTECH_HID_TOKEN_LEN]; } doubtech_hid_token_t;

/** Authenticated, opaque state for one one-based HID connection slot. */
typedef struct {
    uint8_t token[DOUBTECH_HID_TOKEN_LEN];
    bool ready;
    bool subscribed;
    uint32_t notify_tx_successes;
} doubtech_hid_slot_t;

/** Pairing event types delivered to a future host status transport. */
typedef enum {
    DOUBTECH_HID_PAIRING_OPENED,
    DOUBTECH_HID_PAIRING_CHALLENGE,
    DOUBTECH_HID_PAIRING_CLOSED,
    DOUBTECH_HID_BONDED,
    DOUBTECH_HID_PAIRING_REJECTED,
    DOUBTECH_HID_PAIRING_CAPACITY,
    DOUBTECH_HID_PAIRING_TIMEOUT,
    /** A link closed or the host reset; the application can drop per-slot state it cached. */
    DOUBTECH_HID_DISCONNECTED
} doubtech_hid_pairing_event_type_t;

/** Status event; number is the challenge value for CHALLENGE and the one-based slot that closed for DISCONNECTED (0 after a host reset), token only for BONDED. */
typedef struct {
    doubtech_hid_pairing_event_type_t type;
    uint16_t connection_handle;
    uint32_t challenge_id;
    uint32_t number;
    uint64_t deadline_ms;
    doubtech_hid_token_t token;
} doubtech_hid_pairing_event_t;

/** Receives short status events on the NimBLE host thread. */
typedef void (*doubtech_hid_pairing_event_fn)(const doubtech_hid_pairing_event_t *event, void *context);

/** Advertised identity and bond storage; a NULL field selects the library default
 * (name "BLE HID", manufacturer "DoubTech", model "BLE HID", NVS namespace
 * "kvm_bonds", the earlier shipped firmware's namespace). Strings are copied
 * during start and need not outlive it. */
typedef struct {
    const char *device_name;    /**< 1 to DOUBTECH_HID_NAME_MAX_BYTES bytes. */
    const char *manufacturer;   /**< 1 to DOUBTECH_HID_DIS_STRING_MAX_BYTES bytes. */
    const char *model;          /**< 1 to DOUBTECH_HID_DIS_STRING_MAX_BYTES bytes. */
    const char *bond_namespace; /**< 1 to 15 characters of NVS namespace. */
    /** NimBLE bonds that belong to other roles and must not count against the guest bonds; 0 to 8, default 0. */
    uint8_t foreign_bond_reserve;
} doubtech_hid_config_t;

/** Initializes NVS/NimBLE, registers HID, and starts the BLE host task. Returns
 * ESP_ERR_INVALID_ARG, before touching NVS or NimBLE, for an invalid config. */
esp_err_t doubtech_hid_start(const doubtech_hid_config_t *config);
/** True from the NimBLE host sync callback until the next host reset; callable from any task. */
bool doubtech_hid_host_synced(void);
/** Sets a nonblocking status sink before the BLE service starts. */
void doubtech_hid_pairing_set_events(doubtech_hid_pairing_event_fn callback, void *context);
/** Opens pairing for 60 seconds; caller must run on the NimBLE host thread. */
esp_err_t doubtech_hid_pairing_open(void);
/** True while the guest pairing window is open (an expired window closes first);
 * call on the NimBLE host thread. Another role (the input central) refuses its own
 * window while this is true; this library does not refuse a guest window opened
 * during another role's window, so the application must not open both. */
bool doubtech_hid_pairing_active(void);
/** True when the 6-byte address (NimBLE little-endian order, as in ble_addr_t.val)
 * is the identity of a retained guest bond. The type is accepted for symmetry with
 * ble_addr_t but not compared, so a match never depends on public/random typing
 * (fail safe: another role sharing the NimBLE store must never delete or reuse a
 * guest's keys). False before doubtech_hid_start or for NULL. Call only on the
 * NimBLE host thread. */
bool doubtech_hid_peer_is_guest(uint8_t address_type, const uint8_t address[6]);
/** Cancels pairing; caller must run on the NimBLE host thread. */
void doubtech_hid_pairing_cancel(void);
/** Confirms or rejects a pending numeric comparison on the host thread.
 * Guest-initiated comparisons are approved automatically before this can be called. */
esp_err_t doubtech_hid_pairing_confirm(uint32_t challenge_id, bool approved);
/** Copies current pairing state for a host-thread status bridge. */
void doubtech_hid_pairing_snapshot(struct hid_pairing *output);
/** Looks up the current authenticated connection's opaque token on the NimBLE host loop.
 * Clears output and returns false for unbound, disconnected, or unauthenticated peers. */
bool doubtech_hid_current_bond_token(doubtech_hid_token_t *output);
/** Copies all authenticated live slots on the NimBLE host thread. */
bool doubtech_hid_snapshot_slots(doubtech_hid_slot_t output[DOUBTECH_HID_MAX_CONNECTIONS]);
/** Copies all live slots through one bounded host-loop request; zeros on failure. */
bool doubtech_hid_request_slots(doubtech_hid_slot_t output[DOUBTECH_HID_MAX_CONNECTIONS]);
/** Copies retained opaque tokens only; call on the NimBLE host thread. */
bool doubtech_hid_retained_bonds(doubtech_hid_token_t output[DOUBTECH_HID_MAX_BONDS], size_t *count);
/** Removes one bond only after explicit confirmation on the host thread. */
esp_err_t doubtech_hid_pairing_forget(doubtech_hid_token_t token, bool confirmed);
/** Disarms and terminates the current guest after uncertain all-up delivery.
 * Call on the NimBLE host thread; a disconnected channel succeeds. */
esp_err_t doubtech_hid_disconnect_current(void);
/** Disarms and terminates one channel on the NimBLE host thread. */
esp_err_t doubtech_hid_disconnect_slot(uint8_t slot);
/** Queues a coalesced disconnect on NimBLE's host event loop from another task.
 * Returns success when queued; it does not await the radio disconnect. */
esp_err_t doubtech_hid_request_disconnect(void);
/** Reads readiness on the NimBLE host loop within a bounded wait. */
bool doubtech_hid_request_ready(void);
/** Reads readiness for one bounded, one-based GATT channel. */
bool doubtech_hid_request_ready_slot(uint8_t slot);
/** Arms after an all-up baseline on the NimBLE host loop. */
bool doubtech_hid_request_arm(void);
/** Arms one selected channel after its all-up baseline. */
bool doubtech_hid_request_arm_slot(uint8_t slot);
/** Sends all-up reports and disarms on the NimBLE host loop. */
bool doubtech_hid_request_release(void);
/** Releases and disarms one selected channel. */
bool doubtech_hid_request_release_slot(uint8_t slot);
/** Sends one keyboard state through the bounded host-loop bridge. */
bool doubtech_hid_request_keyboard(const uint8_t keys[DOUBTECH_HID_KEYBOARD_REPORT_LEN]);
/** Sends keyboard state to one selected channel. */
bool doubtech_hid_request_keyboard_slot(uint8_t slot, const uint8_t keys[DOUBTECH_HID_KEYBOARD_REPORT_LEN]);
/** Sends relative mouse state through the bounded host-loop bridge. */
bool doubtech_hid_request_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                                int8_t wheel, int8_t pan);
/** Sends pointer state to one selected channel. */
bool doubtech_hid_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy,
                                     int8_t wheel, int8_t pan);
/** Sends one consumer usage through the bounded host-loop bridge. */
bool doubtech_hid_request_consumer(uint16_t usage);
/** Sends consumer usage to one selected channel. */
bool doubtech_hid_request_consumer_slot(uint8_t slot, uint16_t usage);
/** Queues a fail-local disconnect of one selected channel. */
esp_err_t doubtech_hid_request_disconnect_slot(uint8_t slot);
/** Opens a pairing window on the NimBLE host loop within a bounded wait. */
bool doubtech_hid_request_pair_begin(void);
/** Cancels pairing on the NimBLE host loop within a bounded wait. */
bool doubtech_hid_request_pair_cancel(void);
/** Answers the exact challenge on the NimBLE host loop within a bounded wait. */
bool doubtech_hid_request_pair_reply(uint32_t challenge_id, bool approved);
/** Retrieves only a connected authenticated peer's opaque token through a bounded RPC.
 * Always clears output on failure; never returns BLE addresses or key material. */
bool doubtech_hid_request_current_bond_token(uint8_t output[DOUBTECH_HID_TOKEN_LEN]);
/** Copies up to eight retained tokens within a bounded NimBLE host-loop request.
 * Clears output and count on failure; an empty successful inventory is authoritative. */
bool doubtech_hid_request_retained_bonds(uint8_t output[DOUBTECH_HID_MAX_BONDS][DOUBTECH_HID_TOKEN_LEN],
                                         uint8_t *count);
/** Forgets one confirmed opaque bond token on the NimBLE host loop. */
bool doubtech_hid_request_forget_bond(const uint8_t token[DOUBTECH_HID_TOKEN_LEN]);

#endif
