/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes opt-in single-guest NimBLE startup for hardware bring-up. Firmware
 * main remains disarmed; callers must verify board recovery and explicitly
 * start this service before it can advertise or accept a guest. */
#ifndef ESP32_KVM_HID_GUEST_H
#define ESP32_KVM_HID_GUEST_H

#include "esp_err.h"
#include "hid_pairing.h"
#include "hid_report.h"

/** Pairing event types delivered to a future host status transport. */
typedef enum {
    HID_GUEST_PAIRING_OPENED,
    HID_GUEST_PAIRING_CHALLENGE,
    HID_GUEST_PAIRING_CLOSED,
    HID_GUEST_BONDED,
    HID_GUEST_PAIRING_REJECTED
} hid_guest_pairing_event_type_t;

/** Status event; number is present only for CHALLENGE, token only for BONDED. */
typedef struct {
    hid_guest_pairing_event_type_t type;
    uint16_t connection_handle;
    uint32_t number;
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
esp_err_t hid_guest_pairing_confirm(uint16_t connection_handle, bool approved);
/** Copies current pairing state for a host-thread status bridge. */
void hid_guest_pairing_snapshot(hid_pairing_t *output);
/** Removes one bond only after explicit confirmation on the host thread. */
esp_err_t hid_guest_pairing_forget(hid_token_t token, bool confirmed);
/** Disarms and terminates the current guest after uncertain all-up delivery.
 * Call on the NimBLE host thread; a disconnected channel succeeds. */
esp_err_t hid_guest_disconnect_current(void);
/** Queues a coalesced disconnect on NimBLE's host event loop from another task.
 * Returns success when queued; it does not await the radio disconnect. */
esp_err_t hid_guest_request_disconnect(void);
/** Reads readiness on the NimBLE host loop within a bounded wait. */
bool hid_guest_request_ready(void);
/** Arms after an all-up baseline on the NimBLE host loop. */
bool hid_guest_request_arm(void);
/** Sends all-up reports and disarms on the NimBLE host loop. */
bool hid_guest_request_release(void);
/** Sends one keyboard state through the bounded host-loop bridge. */
bool hid_guest_request_keyboard(const uint8_t keys[HID_KEYBOARD_REPORT_LEN]);
/** Sends relative mouse state through the bounded host-loop bridge. */
bool hid_guest_request_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                             int8_t wheel, int8_t pan);
/** Sends one consumer usage through the bounded host-loop bridge. */
bool hid_guest_request_consumer(uint16_t usage);

#endif
