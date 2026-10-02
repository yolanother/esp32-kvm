/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the portable 240-by-240 device status and button-event model.
 * The display task consumes copied status; button sampling only emits requests
 * and never arms or routes HID itself. */
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
} kvm_display_status_t;

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
