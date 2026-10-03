/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the portable device screen state model. It debounces runtime
 * PLUS/BOOT presses and emits requests while preserving the BOOT-at-power-on
 * recovery chord. It derives display text and pairing-window/challenge
 * countdowns from confirmed status and never mutates a HID route directly. */
#include "display_model.h"
#include <stdio.h>
#include <string.h>

#define DEBOUNCE_MS 30u
#define BOOT_HOLD_MS 1000u
#define COPY_TEXT(dst, src) snprintf((dst), sizeof(dst), "%s", (src))

void kvm_display_make_view(const kvm_display_status_t *s, uint64_t now_ms,
                           kvm_display_view_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    if (!s) return;
    v->touch_available = s->touch_available;
    if (s->recovery_required) {
        v->screen = KVM_DISPLAY_RECOVERY;
        COPY_TEXT(v->title, "RECOVERY");
        COPY_TEXT(v->primary, "INPUT PAUSED");
        COPY_TEXT(v->detail, "Connect USB to recover");
    } else if (s->updating) {
        v->screen = KVM_DISPLAY_UPDATE;
        COPY_TEXT(v->title, "UPDATING");
        snprintf(v->primary, sizeof(v->primary), "%u%%",
                 s->update_percent > 100 ? 100u : s->update_percent);
        COPY_TEXT(v->detail, "Do not unplug");
    } else if (s->fault || (s->armed && !s->guest_ready)) {
        v->screen = KVM_DISPLAY_PAUSED;
        COPY_TEXT(v->title, s->armed && !s->guest_ready ? "ROUTING FAULT" : "LOCAL CONTROL");
        COPY_TEXT(v->primary, "INPUT PAUSED");
        COPY_TEXT(v->detail, s->fault ? "Routing fault" :
                  !s->usb_connected ? "USB disconnected" : "Guest not ready");
        COPY_TEXT(v->footer, s->armed && !s->guest_ready ?
                  "Use host to release" : "Reconnect host to resume");
    } else if (s->pairing_state != KVM_DISPLAY_PAIRING_CLOSED) {
        v->screen = KVM_DISPLAY_PAIRING;
        COPY_TEXT(v->title, "PAIR GUEST");
        if (s->pairing_state == KVM_DISPLAY_PAIRING_CHALLENGE &&
            s->pairing_challenge_id && s->pairing_number <= 999999u &&
            s->pairing_deadline_ms > now_ms) {
            snprintf(v->primary, sizeof(v->primary), "%06u", (unsigned)s->pairing_number);
            snprintf(v->detail, sizeof(v->detail), "%llus remaining",
                     (unsigned long long)((s->pairing_deadline_ms - now_ms + 999u) / 1000u));
            COPY_TEXT(v->footer, s->pairing_local_owner ?
                      "Compare, then approve or reject" : "Compare and confirm on host");
        } else if (s->pairing_state == KVM_DISPLAY_PAIRING_WAITING &&
                   s->pairing_deadline_ms > now_ms) {
            COPY_TEXT(v->primary, "WAITING FOR GUEST");
            snprintf(v->detail, sizeof(v->detail), "%llus remaining",
                     (unsigned long long)((s->pairing_deadline_ms - now_ms + 999u) / 1000u));
            COPY_TEXT(v->footer, "Open guest Bluetooth settings");
        } else if (s->pairing_state == KVM_DISPLAY_PAIRING_REJECTED) {
            COPY_TEXT(v->primary, "PAIRING REJECTED");
            COPY_TEXT(v->detail, "Retry pairing");
        } else if (s->pairing_state == KVM_DISPLAY_PAIRING_CAPACITY) {
            COPY_TEXT(v->primary, "NO FREE SLOTS");
            COPY_TEXT(v->detail, "Manage guests on host");
        } else {
            COPY_TEXT(v->primary, "PAIRING EXPIRED");
            COPY_TEXT(v->detail, "Retry pairing");
        }
    } else if (!s->usb_connected) {
        v->screen = KVM_DISPLAY_PAUSED;
        COPY_TEXT(v->title, "LOCAL CONTROL");
        COPY_TEXT(v->primary, "INPUT PAUSED");
        COPY_TEXT(v->detail, "USB disconnected");
        COPY_TEXT(v->footer, s->touch_available ? "Pair here or reconnect host" :
                  "Reconnect host to resume");
    } else if (s->show_guest_list) {
        v->screen = KVM_DISPLAY_GUEST_LIST;
        COPY_TEXT(v->title, "GUESTS");
        COPY_TEXT(v->primary, "SELECT TARGET");
        v->selectable_slots = s->guest_slots > 3 ? 3 : s->guest_slots;
        for (uint8_t i = 0; i < v->selectable_slots; ++i) {
            const char *state = !(s->ready_slots & (1u << i)) ? "OFFLINE" :
                                (s->armed && s->selected_slot == i + 1u) ? "ACTIVE" : "STANDBY";
            snprintf(v->rows[i], sizeof(v->rows[i]), "GUEST %u  %s", i + 1u, state);
        }
        COPY_TEXT(v->footer, "Use host to select");
    } else {
        v->screen = KVM_DISPLAY_ACTIVE;
        COPY_TEXT(v->title, "CURRENT TARGET");
        COPY_TEXT(v->primary, kvm_display_target_label(s));
        snprintf(v->detail, sizeof(v->detail), "USB online | %u guest%s",
                 s->guest_slots, s->guest_slots == 1 ? "" : "s");
        COPY_TEXT(v->footer, "Control changes via host");
    }
}

bool kvm_display_pair_request(const kvm_display_status_t *s, uint64_t now_ms,
                              kvm_display_pair_action_t action,
                              kvm_display_pair_request_t *request)
{
    if (!request) return false;
    memset(request, 0, sizeof(*request));
    if (!s || !s->touch_available || s->fault || s->recovery_required ||
        s->updating || s->armed) return false;
    switch (action) {
    case KVM_DISPLAY_PAIR_BEGIN:
        if (s->pairing_state != KVM_DISPLAY_PAIRING_CLOSED &&
            s->pairing_state != KVM_DISPLAY_PAIRING_REJECTED &&
            s->pairing_state != KVM_DISPLAY_PAIRING_TIMEOUT) return false;
        break;
    case KVM_DISPLAY_PAIR_CANCEL:
        if (!s->pairing_local_owner || s->pairing_state != KVM_DISPLAY_PAIRING_WAITING ||
            s->pairing_deadline_ms <= now_ms) return false;
        break;
    case KVM_DISPLAY_PAIR_APPROVE:
    case KVM_DISPLAY_PAIR_REJECT:
        if (!s->pairing_local_owner || s->pairing_state != KVM_DISPLAY_PAIRING_CHALLENGE ||
            !s->pairing_challenge_id || s->pairing_number > 999999u ||
            s->pairing_deadline_ms <= now_ms) return false;
        request->challenge_id = s->pairing_challenge_id;
        break;
    default: return false;
    }
    request->action = action;
    return true;
}

bool kvm_display_pair_touch_action(const kvm_display_status_t *s,
                                   uint16_t x, uint16_t y,
                                   kvm_display_pair_action_t *action)
{
    if (!s || !action || !s->touch_available || s->fault || s->armed ||
        s->updating || s->recovery_required || x < 12u || x >= 228u ||
        y < 160u || y >= 206u) return false;
    switch (s->pairing_state) {
    case KVM_DISPLAY_PAIRING_CLOSED:
    case KVM_DISPLAY_PAIRING_REJECTED:
    case KVM_DISPLAY_PAIRING_TIMEOUT:
        *action = KVM_DISPLAY_PAIR_BEGIN; return true;
    case KVM_DISPLAY_PAIRING_WAITING:
        if (!s->pairing_local_owner || x >= 115u) return false;
        *action = KVM_DISPLAY_PAIR_CANCEL; return true;
    case KVM_DISPLAY_PAIRING_CHALLENGE:
        if (!s->pairing_local_owner) return false;
        if (x >= 115u && x < 125u) return false;
        *action = x < 115u ? KVM_DISPLAY_PAIR_REJECT : KVM_DISPLAY_PAIR_APPROVE;
        return true;
    default: return false;
    }
}

uint8_t kvm_display_touch_slot(const kvm_display_view_t *v, uint16_t y)
{
    if (!v || !v->touch_available || v->screen != KVM_DISPLAY_GUEST_LIST ||
        y < 44u || y >= 176u) return 0;
    uint8_t index = (uint8_t)((y - 44u) / 44u);
    if (index >= v->selectable_slots || strstr(v->rows[index], "OFFLINE")) return 0;
    return index + 1u;
}

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
        if (!status->guest_ready) return "LOCAL ERROR";
        switch (status->selected_slot) {
        case 1: return "GUEST 1";
        case 2: return "GUEST 2";
        case 3: return "GUEST 3";
        default: return "LOCAL ERROR";
        }
    }
    return status->selected_slot ? "DISARMED" : "HOST";
}
