/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the portable device screen state model. It debounces runtime
 * PLUS/BOOT presses and emits requests while preserving the BOOT-at-power-on
 * recovery chord and never mutating a HID route directly. */
#include "display_model.h"
#include <string.h>

#define DEBOUNCE_MS 30u
#define BOOT_HOLD_MS 1000u

void kvm_display_buttons_init(kvm_display_buttons_t *s, bool plus_down,
                              bool boot_down, uint64_t now_ms)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->plus_stable = s->plus_candidate = plus_down;
    s->boot_stable = s->boot_candidate = boot_down;
    s->boot_blocked_until_release = boot_down;
    s->plus_edge_ms = s->boot_edge_ms = now_ms;
    s->plus_down_ms = s->boot_down_ms = now_ms;
}

kvm_display_event_t kvm_display_buttons_sample(kvm_display_buttons_t *s,
                                                bool plus_down, bool boot_down,
                                                uint64_t now_ms)
{
    if (!s) return KVM_DISPLAY_NO_EVENT;
    kvm_display_event_t event = KVM_DISPLAY_NO_EVENT;
    if (plus_down != s->plus_candidate) {
        s->plus_candidate = plus_down;
        s->plus_edge_ms = now_ms;
    }
    if (plus_down != s->plus_stable && now_ms >= s->plus_edge_ms &&
        now_ms - s->plus_edge_ms >= DEBOUNCE_MS) {
        s->plus_stable = plus_down;
        if (plus_down) {
            s->plus_started = true;
            s->plus_down_ms = now_ms;
        } else if (s->plus_started) {
            s->plus_started = false;
            if (now_ms - s->plus_down_ms < BOOT_HOLD_MS)
                event = KVM_DISPLAY_NEXT_REQUEST;
        }
    }
    if (boot_down != s->boot_candidate) {
        s->boot_candidate = boot_down;
        s->boot_edge_ms = now_ms;
    }
    if (boot_down != s->boot_stable && now_ms >= s->boot_edge_ms &&
        now_ms - s->boot_edge_ms >= DEBOUNCE_MS) {
        s->boot_stable = boot_down;
        if (boot_down) {
            s->boot_started = true;
            s->boot_down_ms = now_ms;
            s->boot_emitted = false;
        } else {
            s->boot_started = false;
            s->boot_blocked_until_release = false;
        }
    }
    if (s->boot_stable && s->boot_started && !s->boot_blocked_until_release &&
        !s->boot_emitted && now_ms >= s->boot_down_ms &&
        now_ms - s->boot_down_ms >= BOOT_HOLD_MS) {
        s->boot_emitted = true;
        return KVM_DISPLAY_EMERGENCY_RELEASE;
    }
    return event;
}

const char *kvm_display_target_label(const kvm_display_status_t *status)
{
    if (!status) return "UNKNOWN";
    if (status->fault) return "LOCAL ERROR";
    if (status->armed) {
        switch (status->selected_slot) {
        case 1: return "GUEST 1";
        case 2: return "GUEST 2";
        case 3: return "GUEST 3";
        default: return "LOCAL ERROR";
        }
    }
    return status->selected_slot ? "DISARMED" : "HOST";
}
