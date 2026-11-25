/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Tracks the device screen's pairing-window ownership and exact pending numeric
 * challenge independently of a USB session. The USB worker uses this state to
 * reject stale touch requests before calling bounded NimBLE host requests. */
#ifndef ESP32_KVM_DISPLAY_PAIRING_H
#define ESP32_KVM_DISPLAY_PAIRING_H

#include "display_model.h"

/** Pairing state owned by the transport worker, with no retained pairing secret. */
typedef struct {
    bool local_owner;
    kvm_display_pairing_state_t state;
    uint32_t challenge_id;
    uint32_t number;
    uint64_t deadline_ms;
} kvm_display_pairing_t;

/** Records a successfully opened 60-second pairing window. */
void kvm_display_pairing_started(kvm_display_pairing_t *pairing, bool local_owner,
                                 uint64_t now_ms);
/** Applies an authenticated HID pairing event, rejecting malformed challenges. */
bool kvm_display_pairing_event(kvm_display_pairing_t *pairing,
                               kvm_display_pairing_state_t state, uint32_t challenge_id,
                               uint32_t number, uint64_t deadline_ms, uint64_t now_ms);
/** Returns whether a queued touch action still matches the current window. */
bool kvm_display_pairing_accept(const kvm_display_pairing_t *pairing,
                                kvm_display_pair_request_t request, uint64_t now_ms);
/** Marks a completed local reply so the challenge cannot be replayed. */
void kvm_display_pairing_replied(kvm_display_pairing_t *pairing, bool approved);
/** Expires a window without exposing a stale comparison number. */
void kvm_display_pairing_expire(kvm_display_pairing_t *pairing, uint64_t now_ms);
/** Clears state after cancellation or emergency release. */
void kvm_display_pairing_clear(kvm_display_pairing_t *pairing);

/* ---- Input-device pairing (keyboard or mouse through the BLE central) ----
 * Worker-owned state for the touchscreen flow. The worker feeds it touch
 * requests, central events (converted to kvm_display_input_event_t) and a
 * periodic tick, and publishes kvm_display_input_pairing_status() into the
 * display snapshot. Central calls go only through the ops, on the worker. */

/** Hard window: the central's 30 s scan window, re-armed at a select the way
 *  the central re-arms its own pairing timer. */
#define KVM_DISPLAY_INPUT_WINDOW_MS 30000u
/** How long a PAIRED or failure result stays before the guest view returns. */
#define KVM_DISPLAY_INPUT_RESULT_MS 4000u
/** How long a refused guest pairing request shows BUSY on the input screen. */
#define KVM_DISPLAY_INPUT_GUEST_BUSY_MS 3000u
/** Scan rows kept (DOUBTECH_HID_CENTRAL_MAX_SCAN_RESULTS); four are shown. */
#define KVM_DISPLAY_INPUT_SCAN_MAX 8u
/** An unconfirmed stop holds the guest gate until the central's own timer has
 *  ended the window: its window deadline plus the central's close guard. */
#define KVM_DISPLAY_INPUT_STOP_MARGIN_MS 5000u
/** Interval between cancel retries while a stop is unconfirmed (each retry is
 *  one bounded RPC, so this keeps host heartbeats served). */
#define KVM_DISPLAY_INPUT_CANCEL_RETRY_MS 1000u

/** Outcome of a central operation as the worker saw it. UNKNOWN: the request
 *  timed out and may still run on the NimBLE host later (window possibly open). */
typedef enum {
    KVM_DISPLAY_INPUT_OP_OK, KVM_DISPLAY_INPUT_OP_REFUSED, KVM_DISPLAY_INPUT_OP_UNKNOWN
} kvm_display_input_op_t;

typedef enum {
    KVM_DISPLAY_INPUT_EVENT_SCAN, KVM_DISPLAY_INPUT_EVENT_PASSKEY,
    KVM_DISPLAY_INPUT_EVENT_PAIRED, KVM_DISPLAY_INPUT_EVENT_FAILED
} kvm_display_input_event_type_t;

/** One central pairing event, copied field for field from the central's event. */
typedef struct {
    kvm_display_input_event_type_t type;
    uint8_t index;
    uint8_t kind;
    char name[24];
    int8_t rssi;
    uint32_t passkey;
    uint8_t reason;
} kvm_display_input_event_t;

/** Central operations; the worker binds doubtech_hid_central_pair_*. cancel
 *  returns true only when the central confirmed it ran (the window is over). */
typedef struct {
    kvm_display_input_op_t (*begin)(void *context);
    kvm_display_input_op_t (*select)(void *context, uint8_t scan_index);
    bool (*cancel)(void *context);
    void *context;
} kvm_display_input_ops_t;

typedef struct {
    bool used;
    uint8_t index;
    uint8_t kind;
    int8_t rssi;
    char name[24];
} kvm_display_input_scan_t;

typedef struct {
    kvm_display_input_ops_t ops;
    kvm_display_input_state_t state;
    bool connecting;
    bool refused;               /* RESULT from a refused begin/select: the central's reason may refine it */
    bool stopping;              /* RESULT whose central window is not confirmed closed */
    uint8_t busy_cause;         /* KVM_DISPLAY_INPUT_BUSY_* for a BUSY result */
    uint64_t stop_deadline_ms;  /* the central's own timer has ended the window by then */
    uint64_t cancel_retry_ms;
    uint32_t window;
    uint64_t deadline_ms;       /* window deadline, or the RESULT dismiss time */
    uint64_t guest_busy_until_ms;
    uint32_t passkey;
    uint8_t reason;
    uint8_t kind;
    char name[24];
    kvm_display_input_scan_t scan[KVM_DISPLAY_INPUT_SCAN_MAX];
} kvm_display_input_pairing_t;

/** Starts closed, bound to the central operations. */
void kvm_display_input_pairing_init(kvm_display_input_pairing_t *pairing,
                                    kvm_display_input_ops_t ops);
/** Applies one touch request. BEGIN is refused with a BUSY result (the central
 *  untouched) while host_armed (the PC is driving a guest; an open, unarmed
 *  host session is fine) or guest_pairing is true. A begin or select whose
 *  RPC timed out, or a cancel the central did not confirm, leaves the window
 *  "possibly open": a BUSY/failed result is shown, the guest gate stays closed
 *  and cancel is retried until confirmed or the stop deadline. Returns true
 *  when the request started, selected or cancelled something. */
bool kvm_display_input_pairing_request(kvm_display_input_pairing_t *pairing,
                                       kvm_display_input_request_t request, uint64_t now_ms,
                                       bool host_armed, bool guest_pairing);
/** Applies one central event; events outside a matching state are ignored. */
void kvm_display_input_pairing_event(kvm_display_input_pairing_t *pairing,
                                     const kvm_display_input_event_t *event, uint64_t now_ms);
/** Enforces the hard deadline and exclusivity: an open window past its
 *  deadline, or with the router now armed by the host, is cancelled on the
 *  central and shown as TIMED OUT / BUSY (an unarmed session opening does not
 *  touch it). Retries an unconfirmed cancel; a result past its time closes
 *  once the window is known to be over. */
void kvm_display_input_pairing_tick(kvm_display_input_pairing_t *pairing, uint64_t now_ms,
                                    bool host_armed);
/** True from BEGIN until the result is dismissed and the central window is
 *  known closed: no guest window may open. */
bool kvm_display_input_pairing_blocks_guest(const kvm_display_input_pairing_t *pairing);
/** Records a guest pairing request refused for the input window (shown as BUSY). */
void kvm_display_input_pairing_guest_refused(kvm_display_input_pairing_t *pairing, uint64_t now_ms);
/** Writes the input_pair_* snapshot fields: at most four rows, strongest RSSI
 *  first (ties by scan index), each with its stable central scan index. */
void kvm_display_input_pairing_status(const kvm_display_input_pairing_t *pairing, uint64_t now_ms,
                                      kvm_display_status_t *status);

#endif
