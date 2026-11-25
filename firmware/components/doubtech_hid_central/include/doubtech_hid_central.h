/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Public API of the doubtech_hid_central component: a BLE HID-over-GATT client
 * that pairs and connects at most one keyboard and one mouse while the same
 * NimBLE host keeps serving guests through doubtech_hid. It never initializes
 * the host: doubtech_hid owns nimble_port and the host task, and this component
 * starts only after doubtech_hid_host_synced() is true.
 *
 * Threading: on_input and on_event run on the NimBLE host thread. They must
 * only copy and enqueue; they must not block or call back into this API. The
 * pairing, forget and query functions may be called from any task except the
 * NimBLE host thread; they run on the host loop through a bounded request. */
#ifndef DOUBTECH_HID_CENTRAL_H
#define DOUBTECH_HID_CENTRAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Input-device kinds; equal to dthid_input_kind_t in central_registry.h. */
#define DOUBTECH_HID_CENTRAL_KEYBOARD 1
#define DOUBTECH_HID_CENTRAL_MOUSE 2

/** Failure reasons carried by DTHID_CENTRAL_PAIR_FAILED. */
#define DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED 1
#define DOUBTECH_HID_CENTRAL_REASON_TIMEOUT 2
#define DOUBTECH_HID_CENTRAL_REASON_AUTH_FAILED 3
#define DOUBTECH_HID_CENTRAL_REASON_BUSY 4
#define DOUBTECH_HID_CENTRAL_REASON_CAPACITY 5

/** Most scan results one pairing window lists (deduplicated by address). */
#define DOUBTECH_HID_CENTRAL_MAX_SCAN_RESULTS 8
/** Length of the scan window opened by doubtech_hid_central_pair_begin. */
#define DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS 30000u

/** One input report from a connected device. A keyboard fills keys (the 8-byte
 * boot report); a mouse fills buttons, dx, dy and wheel. lost is true, with all
 * other fields zero, when a device that delivered input disconnects; the
 * consumer must then release every key and button it holds for that kind. */
typedef struct {
    uint8_t kind;
    uint8_t keys[8];
    uint8_t buttons;
    int16_t dx, dy;
    int8_t wheel;
    bool lost;
} doubtech_hid_central_input_t;

typedef enum {
    DTHID_CENTRAL_SCAN_RESULT,
    DTHID_CENTRAL_PASSKEY,
    DTHID_CENTRAL_PAIRED,
    DTHID_CENTRAL_PAIR_FAILED,
    DTHID_CENTRAL_CONNECTED,
    DTHID_CENTRAL_DISCONNECTED
} doubtech_hid_central_event_type_t;

/** index: scan row for SCAN_RESULT (a repeated index updates that row).
 * kind: the device kind (0 while a scan result's kind is unknown).
 * name: NUL-terminated advertised name, possibly empty.
 * rssi: SCAN_RESULT only. passkey: PASSKEY only, sent for the passkey-entry
 * method (the owner types it on the keyboard); a numeric comparison is accepted
 * silently because the owner already chose the device.
 * reason: PAIR_FAILED only, one of DOUBTECH_HID_CENTRAL_REASON_*. */
typedef struct {
    doubtech_hid_central_event_type_t type;
    uint8_t index;
    uint8_t kind;
    char name[24];
    int8_t rssi;
    uint32_t passkey;
    uint8_t reason;
} doubtech_hid_central_event_t;

typedef struct {
    void (*on_input)(const doubtech_hid_central_input_t *input, void *context);
    void (*on_event)(const doubtech_hid_central_event_t *event, void *context);
    void *context;
    /** NVS namespace for input-device records; NULL selects "dthid_inputs". */
    const char *store_namespace;
} doubtech_hid_central_config_t;

/** Loads the stored devices and prepares the host-loop bridge. Returns
 * ESP_ERR_INVALID_STATE before doubtech_hid_host_synced() or on a second call,
 * ESP_ERR_INVALID_ARG for a NULL config or an invalid namespace. It connects
 * nothing by itself. */
esp_err_t doubtech_hid_central_start(const doubtech_hid_central_config_t *config);

/** Opens a 30 s scan for HID keyboards and mice with sm_mitm cleared for the
 * window. Refused (ESP_ERR_INVALID_STATE plus PAIR_FAILED reason 4) while guest
 * pairing is open or another input pairing runs, and (reason 5) when a keyboard
 * and a mouse are already stored. Exclusivity is enforced one way only: this
 * call checks doubtech_hid_pairing_active(), but doubtech_hid does not refuse a
 * guest window opened during an input window. The application must not open
 * both (guest bonding still requires an authenticated link either way). */
esp_err_t doubtech_hid_central_pair_begin(void);
/** Ends the window silently; a half-paired device is disconnected and its new bond removed. */
void doubtech_hid_central_pair_cancel(void);
/** Connects and pairs the scan row index. The result arrives as PAIRED then
 * CONNECTED, or PAIR_FAILED. Replacing a stored kind requires forget first. */
esp_err_t doubtech_hid_central_pair_select(uint8_t index);

/** Disconnects and removes the stored device of that kind and its bond.
 * ESP_ERR_NOT_FOUND when none is stored. */
esp_err_t doubtech_hid_central_forget(uint8_t kind);
/** True while a device of that kind is connected and delivering input; any task. */
bool doubtech_hid_central_connected(uint8_t kind);

#endif
