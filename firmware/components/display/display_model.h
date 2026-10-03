/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the portable 240-by-240 device status, derived screen, and button
 * model. Rendering consumes copied confirmed status; button and touch helpers
 * emit requests and never arm or route HID themselves. */
#ifndef ESP32_KVM_DISPLAY_MODEL_H
#define ESP32_KVM_DISPLAY_MODEL_H

#include <stdbool.h>
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
} kvm_display_status_t;

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

/** One of the five 240-by-240 device views. */
typedef enum {
    KVM_DISPLAY_ACTIVE, KVM_DISPLAY_GUEST_LIST, KVM_DISPLAY_PAIRING,
    KVM_DISPLAY_PAUSED, KVM_DISPLAY_UPDATE, KVM_DISPLAY_RECOVERY
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
} kvm_display_view_t;

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
/** Formats a short target label, including an explicit disarmed state. */
const char *kvm_display_target_label(const kvm_display_status_t *status);

#endif
