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
    uint8_t hub_target;  /* device-input target: 0 This PC (a stub until stage 3), 1 to 3 guests */
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
    /* Stored input devices, for the DEVICES list (filled in every snapshot). */
    uint8_t input_dev_stored;          /* bit (1 << kind) per stored keyboard / mouse */
    char input_dev_names[2][25];       /* [0] keyboard, [1] mouse; empty when unnamed */
    uint8_t input_dev_confirm;         /* DEVICES: kind awaiting the confirm tap, 0 none */
    uint8_t input_dev_notice;          /* DEVICES footer: KVM_DISPLAY_INPUT_NOTICE_* */
    uint8_t input_dev_notice_kind;     /* the kind the notice is about */
    /* USB mode facts, filled by the USB worker (kvm_transport_display_usb).
       All 0 is today's serial-mode screen; only usb_mode == 1 changes it. */
    uint8_t usb_mode;                  /* 0 USB Serial/JTAG (legacy), 1 composite USB HID */
    uint8_t pc_hid;                    /* composite: 0 not enumerated, 1 suspended, 2 ready */
    uint8_t usb_reason;                /* this boot's KVM_DISPLAY_USB_REASON_* */
    uint8_t usb_result;                /* worker's answer to a request: KVM_DISPLAY_USB_RESULT_* */
    /* USB mode screen, display-local (kvm_display_usb_status); the worker
       leaves these 0. */
    bool usb_screen;                   /* the screen is open */
    uint8_t usb_confirm;               /* action awaiting its confirm tap, 0 none */
    uint8_t usb_notice;                /* KVM_DISPLAY_USB_NOTICE_* */
} kvm_display_status_t;

/** Boot reasons, equal to kvm_usb_reason_t (usb_mode.h; the transport checks it). */
#define KVM_DISPLAY_USB_REASON_DEFAULT 0u
#define KVM_DISPLAY_USB_REASON_STORED 1u
#define KVM_DISPLAY_USB_REASON_NOT_BUILT 2u
#define KVM_DISPLAY_USB_REASON_FORCED_BY_BUILD 3u
#define KVM_DISPLAY_USB_REASON_SERIAL_ONCE 4u
#define KVM_DISPLAY_USB_REASON_ESCAPE_BUTTON 5u
#define KVM_DISPLAY_USB_REASON_ABNORMAL_RESET 6u
#define KVM_DISPLAY_USB_REASON_UNCONFIRMED_LIMIT 7u

/** USB mode requests from the screen. The display only produces them (through
 *  kvm_display_set_usb_fn); the USB worker decides and acts. */
typedef enum {
    KVM_DISPLAY_USB_USE_COMPOSITE = 1, KVM_DISPLAY_USB_USE_SERIAL = 2,
    KVM_DISPLAY_USB_SERIAL_ONCE = 3
} kvm_display_usb_request_t;

/** The worker's answer to the latest request (kvm_display_status_t.usb_result). */
#define KVM_DISPLAY_USB_RESULT_NONE 0u
#define KVM_DISPLAY_USB_RESULT_NOT_BUILT 1u   /* composite refused: not in this build */
#define KVM_DISPLAY_USB_RESULT_BUSY 2u        /* a guest is armed or a guest window is open */
#define KVM_DISPLAY_USB_RESULT_RESTARTING 3u  /* released everything, restarting */
#define KVM_DISPLAY_USB_RESULT_FAILED 4u      /* the mode could not be stored; not restarted */
/** How long the worker keeps a result in the snapshot. */
#define KVM_DISPLAY_USB_RESULT_MS 4000u

/** Display-local footer notices on the USB mode screen. */
#define KVM_DISPLAY_USB_NOTICE_NONE 0u
#define KVM_DISPLAY_USB_NOTICE_NOT_BUILT 1u   /* composite tapped on a build without it */
#define KVM_DISPLAY_USB_NOTICE_SENT 2u        /* a confirmed request went to the worker */
/** The second tap on the same action sends it only within this time. */
#define KVM_DISPLAY_USB_CONFIRM_MS 5000u
/** A notice stays this long. */
#define KVM_DISPLAY_USB_NOTICE_MS 4000u
/** The screen closes after this long without a touch. */
#define KVM_DISPLAY_USB_IDLE_MS 30000u

/** Display-task state of the USB mode screen (never shared with the worker). */
typedef struct {
    bool open;
    uint8_t confirm;             /* kvm_display_usb_request_t awaiting the second tap */
    uint64_t confirm_until_ms;
    uint8_t notice;              /* KVM_DISPLAY_USB_NOTICE_* */
    uint64_t notice_until_ms;
    uint64_t idle_until_ms;
} kvm_display_usb_ui_t;

/** What a press did on the USB mode screen or its entry. */
typedef enum {
    KVM_DISPLAY_USB_TOUCH_NONE,     /* not a USB zone: other handlers may take it */
    KVM_DISPLAY_USB_TOUCH_HANDLED,  /* the screen changed; nothing to send */
    KVM_DISPLAY_USB_TOUCH_OPENED,   /* opened from the device list: close that list */
    KVM_DISPLAY_USB_TOUCH_SEND      /* a confirmed request: pass *request to the worker */
} kvm_display_usb_touch_t;

/** Input-device screen states (kvm_display_status_t.input_pair_state). DEVICES
 *  is the list behind the entry: one row per kind (tap twice to forget) and
 *  "Add keyboard or mouse", which opens the pairing window. */
typedef enum {
    KVM_DISPLAY_INPUT_CLOSED = 0, KVM_DISPLAY_INPUT_SCANNING = 1,
    KVM_DISPLAY_INPUT_PASSKEY = 2, KVM_DISPLAY_INPUT_RESULT = 3,
    KVM_DISPLAY_INPUT_DEVICES = 4
} kvm_display_input_state_t;

/** DEVICES footer notices. */
#define KVM_DISPLAY_INPUT_NOTICE_NONE 0u
#define KVM_DISPLAY_INPUT_NOTICE_FORGOTTEN 1u
#define KVM_DISPLAY_INPUT_NOTICE_FAILED 2u
/** The forget request timed out and may still run: settled by the next refresh. */
#define KVM_DISPLAY_INPUT_NOTICE_FORGETTING 3u
/** DEVICES rows: keyboard, mouse, then "Add keyboard or mouse". */
#define KVM_DISPLAY_INPUT_DEVICE_ROWS 3u
#define KVM_DISPLAY_INPUT_ADD_ROW 2u
/** The list's fourth row (y 176..220) opens the USB mode screen; it is not a
 *  device-list touch zone (kvm_display_usb_touch handles it). */
#define KVM_DISPLAY_INPUT_USB_ROW 3u

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

/** Touch actions on the input-device screens. DEVICES_OPEN (the entry) shows
 *  the list; FORGET (row 0 keyboard, 1 mouse) asks to confirm, and the same
 *  row tapped again within KVM_DISPLAY_INPUT_FORGET_CONFIRM_MS forgets it;
 *  BEGIN (the list's "Add" row) opens the pairing window; CANCEL closes. */
typedef enum {
    KVM_DISPLAY_INPUT_PAIR_BEGIN, KVM_DISPLAY_INPUT_PAIR_SELECT, KVM_DISPLAY_INPUT_PAIR_CANCEL,
    KVM_DISPLAY_INPUT_DEVICES_OPEN, KVM_DISPLAY_INPUT_FORGET
} kvm_display_input_action_t;

/** A touch captured with the window and the central scan index it showed, so a
 *  stale or re-sorted list can never select a different device. */
typedef struct {
    kvm_display_input_action_t action;
    uint8_t row;         /* displayed row (SELECT; FORGET: 0 keyboard, 1 mouse) */
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
    KVM_DISPLAY_INPUT_PAIR, KVM_DISPLAY_USB_MODE
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
    bool footer_hint; /* footer is the "This PC: use Ctrl+Alt+1-3" hint: shown even when dimmed */
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
/** Maps a press to an input-device action: the entry beside the guest
 *  settings (DEVICES_OPEN), on the list a device row (FORGET, *row 0 keyboard
 *  or 1 mouse) or the "Add" row (BEGIN), a displayed scan row (SELECT, *row is
 *  the displayed row) or the close control (CANCEL). False for any other press. */
bool kvm_display_input_touch_action(const kvm_display_status_t *status,
                                    uint16_t x, uint16_t y,
                                    kvm_display_input_action_t *action, uint8_t *row);
/** Validates an input action against the displayed snapshot and captures the
 *  window and, for SELECT, the central scan index of that displayed row. The
 *  list's actions (FORGET, BEGIN from the list) are refused while the host has
 *  a guest armed or a guest pairing window is open; FORGET also needs a stored
 *  device on that row. */
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
/** Writes the footer badge. Serial mode (usb_mode != 1), as before: "USB on"
 *  or "USB off" for the host session, then " | KB MS" for connected input
 *  devices, e.g. "USB off | KB MS". Composite mode (usb_mode == 1): "PC on",
 *  "PC asleep" or "USB off" (not enumerated) from pc_hid, then " | App" with a
 *  host session, then " | KB MS", e.g. "PC on | App | KB". Always
 *  NUL-terminates when n > 0; truncates safely. */
void kvm_display_badge_text(const kvm_display_status_t *status, char *out, size_t n);
/** Formats a short target label, including an explicit disarmed state. */
const char *kvm_display_target_label(const kvm_display_status_t *status);

/* ---- USB mode screen (spec Decision 22) ----
 * Opened from the INPUT DEVICES list's "USB MODE" row, under the list's gate
 * (touch, no guest armed, no guest pairing window). It shows the mode, this
 * boot's reason and three actions; each needs a second tap on the same row
 * within KVM_DISPLAY_USB_CONFIRM_MS, a tap on another row re-arms that one.
 * USE_COMPOSITE on an image without composite support (reason NOT_BUILT or
 * FORCED_BY_BUILD) only shows "Not in this build". The display task owns the
 * state, projects it with kvm_display_usb_status before kvm_display_make_view,
 * and only hands confirmed requests to the worker; it never calls USB code. */

/** Short on-screen text for a KVM_DISPLAY_USB_REASON_* value ("Unknown" otherwise). */
const char *kvm_display_usb_reason_text(uint8_t reason);
/** Starts closed. */
void kvm_display_usb_ui_init(kvm_display_usb_ui_t *ui);
/** Closes the screen when its gate fails (guest armed, guest pairing, an input
 *  pairing window or result, fault, update, recovery, no touch) or after
 *  KVM_DISPLAY_USB_IDLE_MS without a touch; expires the confirm and notice. */
void kvm_display_usb_tick(kvm_display_usb_ui_t *ui, const kvm_display_status_t *status,
                          uint64_t now_ms);
/** Writes usb_screen, usb_confirm and usb_notice (still-valid ones only). */
void kvm_display_usb_status(const kvm_display_usb_ui_t *ui, uint64_t now_ms,
                            kvm_display_status_t *status);
/** Applies one press against the projected snapshot: the device list's USB
 *  row (OPENED), and on the open screen the close control or an action row
 *  (HANDLED, or SEND with *request set on the confirming tap). NONE for any
 *  other press and whenever the gate fails. */
kvm_display_usb_touch_t kvm_display_usb_touch(kvm_display_usb_ui_t *ui,
                                              const kvm_display_status_t *status,
                                              uint16_t x, uint16_t y, uint64_t now_ms,
                                              kvm_display_usb_request_t *request);

#endif
