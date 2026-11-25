/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the portable 240-by-240 device status, derived screen, and button
 * model. Rendering consumes copied confirmed status; button and touch helpers
 * emit requests and never arm or route HID themselves. */
#ifndef ESP32_KVM_DISPLAY_MODEL_H
#define ESP32_KVM_DISPLAY_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Logical status shown by the device screen. */
typedef struct {
    bool usb_connected;
    bool armed;
    bool guest_ready;
    bool fault;
    uint8_t selected_slot;
    uint32_t generation;
    uint8_t guest_slots;
    uint8_t ready_slots;
    uint8_t ready_mask;
    uint8_t occupied_mask;
    uint8_t paired_guests;
    char guest_names[3][25];
    bool show_guest_list;
    bool touch_available;
    uint8_t pairing_state;
    bool pairing_local_owner;
    uint32_t pairing_challenge_id;
    uint32_t pairing_number;
    uint64_t pairing_deadline_ms;
    bool updating;
    uint8_t update_percent;
    bool recovery_required;
    /* Device-owned input facts, filled by the USB worker; not yet rendered. */
    bool host_session;   /* the router holds a live host session */
    bool input_kbd;      /* a paired keyboard is connected */
    bool input_mouse;    /* a paired mouse is connected */
    uint8_t hub_target;  /* device-input target: 0 This PC, 1 to 3 guests */
    /* Input-device pairing snapshot, built by the USB worker
       (kvm_display_input_pairing_status); the display never asks the central. */
    uint8_t input_pair_state;          /* kvm_display_input_state_t */
    uint8_t input_pair_count;          /* rows shown, strongest signal first */
    char input_pair_rows[4][25];
    uint8_t input_pair_index[4];       /* the central's stable scan index per row */
    uint32_t input_pair_passkey;       /* PASSKEY only, 0 to 999999 */
    uint8_t input_pair_reason;         /* RESULT: 0 paired, 1 to 5 failure reasons */
    uint8_t input_pair_kind;           /* KVM_DISPLAY_INPUT_KEYBOARD / _MOUSE, 0 unknown */
    bool input_pair_connecting;        /* SCANNING after a row was selected */
    bool input_pair_guest_busy;        /* a guest pairing request was just refused */
    char input_pair_name[25];          /* the selected or paired device */
    uint32_t input_pair_window;        /* identifies the window a touch belongs to */
    uint64_t input_pair_deadline_ms;   /* hard window deadline (scan, connect, passkey) */
    bool input_pair_stopping;          /* RESULT: the central window is not confirmed closed yet */
    uint8_t input_pair_busy_cause;     /* RESULT BUSY: KVM_DISPLAY_INPUT_BUSY_* */
} kvm_display_status_t;

/** Input-device pairing screen states (kvm_display_status_t.input_pair_state). */
typedef enum {
    KVM_DISPLAY_INPUT_CLOSED = 0, KVM_DISPLAY_INPUT_SCANNING = 1,
    KVM_DISPLAY_INPUT_PASSKEY = 2, KVM_DISPLAY_INPUT_RESULT = 3
} kvm_display_input_state_t;

/** Device kinds, equal to DOUBTECH_HID_CENTRAL_KEYBOARD / _MOUSE. */
#define KVM_DISPLAY_INPUT_KEYBOARD 1u
#define KVM_DISPLAY_INPUT_MOUSE 2u
/** Result reasons, equal to DOUBTECH_HID_CENTRAL_REASON_* (0 means paired). */
#define KVM_DISPLAY_INPUT_REASON_NOT_SUPPORTED 1u
#define KVM_DISPLAY_INPUT_REASON_TIMEOUT 2u
#define KVM_DISPLAY_INPUT_REASON_AUTH_FAILED 3u
#define KVM_DISPLAY_INPUT_REASON_BUSY 4u
#define KVM_DISPLAY_INPUT_REASON_CAPACITY 5u
/** Display-only reason: the owner cancelled and the central has not confirmed yet. */
#define KVM_DISPLAY_INPUT_REASON_CANCELLED 6u
/** Why a BUSY result was shown. */
#define KVM_DISPLAY_INPUT_BUSY_CENTRAL 0u  /* the central refused or did not answer */
#define KVM_DISPLAY_INPUT_BUSY_HOST 1u     /* the PC app is controlling a guest (router armed) */
#define KVM_DISPLAY_INPUT_BUSY_GUEST 2u    /* a guest pairing window is open */

/** Touch actions on the input-device pairing screen. */
typedef enum {
    KVM_DISPLAY_INPUT_PAIR_BEGIN, KVM_DISPLAY_INPUT_PAIR_SELECT, KVM_DISPLAY_INPUT_PAIR_CANCEL
} kvm_display_input_action_t;

/** A touch captured with the window and the central scan index it showed, so a
 *  stale or re-sorted list can never select a different device. */
typedef struct {
    kvm_display_input_action_t action;
    uint8_t row;         /* displayed row (SELECT) */
    uint8_t scan_index;  /* the central's scan index of that row (SELECT) */
    uint32_t window;
} kvm_display_input_request_t;

/** Pairing states mirrored from the negotiated firmware status protocol. */
typedef enum {
    KVM_DISPLAY_PAIRING_CLOSED = 0, KVM_DISPLAY_PAIRING_WAITING = 1,
    KVM_DISPLAY_PAIRING_CHALLENGE = 2, KVM_DISPLAY_PAIRING_REJECTED = 3,
    KVM_DISPLAY_PAIRING_CAPACITY = 4, KVM_DISPLAY_PAIRING_TIMEOUT = 5
} kvm_display_pairing_state_t;

/** Device touch actions sent to the USB worker for bounded HID handling. */
typedef enum {
    KVM_DISPLAY_PAIR_BEGIN, KVM_DISPLAY_PAIR_CANCEL,
    KVM_DISPLAY_PAIR_APPROVE, KVM_DISPLAY_PAIR_REJECT
} kvm_display_pair_action_t;

/** Exact challenge identity captured with a displayed touch action. */
typedef struct {
    kvm_display_pair_action_t action;
    uint32_t challenge_id;
} kvm_display_pair_request_t;

/** One of the 240-by-240 device views. */
typedef enum {
    KVM_DISPLAY_ACTIVE, KVM_DISPLAY_GUEST_LIST, KVM_DISPLAY_PAIRING,
    KVM_DISPLAY_PAUSED, KVM_DISPLAY_UPDATE, KVM_DISPLAY_RECOVERY,
    KVM_DISPLAY_INPUT_PAIR
} kvm_display_screen_t;

/** Fixed-size view copied by LVGL; no pairing secret is persisted or logged. */
typedef struct {
    kvm_display_screen_t screen;
    char title[24];
    char primary[32];
    char detail[48];
    char footer[48];
    char rows[3][32];
    uint8_t selectable_slots;
    bool touch_available;
    char card_labels[4][25];
    uint8_t card_visible_mask;
    uint8_t card_ready_mask;
    uint8_t selected_card;
    bool show_guest_settings;
    bool usb_dim;  /* no host session: render the view with a dimmed "USB off" badge */
    bool show_input_entry;      /* "Add keyboard or mouse", beside the guest settings */
    bool input_cancel;          /* INPUT_PAIR: the close/cancel control is shown */
    uint8_t input_row_count;    /* INPUT_PAIR: selectable device rows */
    char input_rows[4][25];
} kvm_display_view_t;

/* Touch geometry of the input-device pairing flow (minimum target 42 px, as
 * the guest settings gear). The entry sits left of the gear (190..232). */
#define KVM_DISPLAY_INPUT_ENTRY_X 142u
#define KVM_DISPLAY_INPUT_ENTRY_Y 194u
#define KVM_DISPLAY_INPUT_ENTRY_SIZE 42u
#define KVM_DISPLAY_INPUT_CANCEL_X 190u   /* close/cancel: x 190..232, y 0..44 */
#define KVM_DISPLAY_INPUT_CANCEL_W 42u
#define KVM_DISPLAY_INPUT_CANCEL_H 44u
#define KVM_DISPLAY_INPUT_ROW_X 8u        /* rows: x 8..232, 44 px from y 44 */
#define KVM_DISPLAY_INPUT_ROW_W 224u
#define KVM_DISPLAY_INPUT_ROW_Y 44u
#define KVM_DISPLAY_INPUT_ROW_H 44u

/** Derives visible text from confirmed status and a monotonic timestamp. */
void kvm_display_make_view(const kvm_display_status_t *status, uint64_t now_ms,
                           kvm_display_view_t *view);
/** Validates a touch action against the displayed state and captures its challenge ID. */
bool kvm_display_pair_request(const kvm_display_status_t *status, uint64_t now_ms,
                              kvm_display_pair_action_t action,
                              kvm_display_pair_request_t *request);
/** Maps a press in the visible action row to its specific pairing action. */
bool kvm_display_pair_touch_action(const kvm_display_status_t *status,
                                   uint16_t x, uint16_t y,
                                   kvm_display_pair_action_t *action);
/** Maps a press to an input-device pairing action: the entry beside the guest
 *  settings (BEGIN), a displayed device row (SELECT, *row is the displayed
 *  row) or the close control (CANCEL). False for any other press. */
bool kvm_display_input_touch_action(const kvm_display_status_t *status,
                                    uint16_t x, uint16_t y,
                                    kvm_display_input_action_t *action, uint8_t *row);
/** Validates an input action against the displayed snapshot and captures the
 *  window and, for SELECT, the central scan index of that displayed row. */
bool kvm_display_input_request(const kvm_display_status_t *status,
                               kvm_display_input_action_t action, uint8_t row,
                               kvm_display_input_request_t *request);
/** Returns the ready slot touched in a guest row, or zero if unavailable. */
uint8_t kvm_display_touch_slot(const kvm_display_view_t *view, uint16_t y);
/** Cycles through live guests and then Local; zero means request Local. */
uint8_t kvm_display_next_ready_slot(uint8_t ready_mask, uint8_t current_slot);

/** Request from a runtime button, never a direct route mutation. */
typedef enum {
    KVM_DISPLAY_NO_EVENT = 0,
    KVM_DISPLAY_NEXT_REQUEST,
    KVM_DISPLAY_EMERGENCY_RELEASE
} kvm_display_event_t;

/** Debounced state for active-low PLUS and BOOT inputs. */
typedef struct {
    bool plus_stable;
    bool boot_stable;
    bool plus_candidate;
    bool boot_candidate;
    bool plus_started;
    bool boot_started;
    bool boot_blocked_until_release;
    bool boot_emitted;
    uint64_t plus_edge_ms;
    uint64_t boot_edge_ms;
    uint64_t plus_down_ms;
    uint64_t boot_down_ms;
} kvm_display_buttons_t;

/** Initializes button sampling; a BOOT held during startup cannot trigger runtime release. */
void kvm_display_buttons_init(kvm_display_buttons_t *state, bool plus_down,
                              bool boot_down, uint64_t now_ms);
/** Samples buttons and returns at most one event, prioritizing emergency release. */
kvm_display_event_t kvm_display_buttons_sample(kvm_display_buttons_t *state,
                                                bool plus_down, bool boot_down,
                                                uint64_t now_ms);
/** Writes the footer badge, e.g. "USB off | KB MS": USB state, then KB/MS when connected.
 *  Always NUL-terminates when n > 0; truncates safely. */
void kvm_display_badge_text(const kvm_display_status_t *status, char *out, size_t n);
/** Formats a short target label, including an explicit disarmed state. */
const char *kvm_display_target_label(const kvm_display_status_t *status);

#endif
