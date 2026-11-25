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

static bool guest_pairing_idle(const kvm_display_status_t *s)
{
    return s->pairing_state == KVM_DISPLAY_PAIRING_CLOSED ||
           s->pairing_state == KVM_DISPLAY_PAIRING_REJECTED ||
           s->pairing_state == KVM_DISPLAY_PAIRING_TIMEOUT;
}

/* The "Add keyboard or mouse" entry shares the guest settings gear's gate. */
static bool input_entry_available(const kvm_display_status_t *s)
{
    return s->touch_available && s->paired_guests > 0 && !s->fault && !s->armed &&
           !s->updating && !s->recovery_required && guest_pairing_idle(s) &&
           s->input_pair_state == KVM_DISPLAY_INPUT_CLOSED;
}

/* The input screen is shown unless recovery, update or a pause outranks it. */
static bool input_screen_shown(const kvm_display_status_t *s)
{
    return s->input_pair_state >= KVM_DISPLAY_INPUT_SCANNING &&
           s->input_pair_state <= KVM_DISPLAY_INPUT_DEVICES &&
           !s->recovery_required && !s->updating && !s->fault &&
           !(s->armed && !s->guest_ready);
}

static const char *input_kind_text(uint8_t kind)
{
    return kind == KVM_DISPLAY_INPUT_KEYBOARD ? "KEYBOARD" :
           kind == KVM_DISPLAY_INPUT_MOUSE ? "MOUSE" : "DEVICE";
}

/* The device list's own actions: the entry's gate, minus the closed state. */
static bool input_list_usable(const kvm_display_status_t *s)
{
    return s->input_pair_state == KVM_DISPLAY_INPUT_DEVICES && s->touch_available &&
           !s->armed && guest_pairing_idle(s) && input_screen_shown(s);
}

static bool input_dev_stored(const kvm_display_status_t *s, uint8_t kind)
{
    return (s->input_dev_stored & (1u << kind)) != 0;
}

/* The stored device's name, or its kind when it advertised none. */
static const char *input_dev_label(const kvm_display_status_t *s, uint8_t kind)
{
    const char *name = s->input_dev_names[kind - 1u];
    return name[0] ? name : input_kind_text(kind);
}

/* Joins up to three parts into dst, truncating to fit (a long name is cut,
 * the row label dots it). */
static void join_text(char *dst, size_t n, const char *a, const char *b, const char *c)
{
    const char *parts[3] = {a, b, c};
    size_t at = 0;
    for (unsigned i = 0; i < 3u; ++i)
        for (const char *p = parts[i]; p && *p && at + 1u < n; ++p) dst[at++] = *p;
    dst[at] = '\0';
}

static void make_devices_view(const kvm_display_status_t *s, kvm_display_view_t *v)
{
    COPY_TEXT(v->title, "INPUT DEVICES");
    COPY_TEXT(v->footer, "Tap a device to forget");
    v->input_row_count = KVM_DISPLAY_INPUT_DEVICE_ROWS;
    for (uint8_t kind = KVM_DISPLAY_INPUT_KEYBOARD; kind <= KVM_DISPLAY_INPUT_MOUSE; ++kind) {
        char *row = v->input_rows[kind - 1u];
        const char *name = s->input_dev_names[kind - 1u];
        if (!input_dev_stored(s, kind))
            join_text(row, sizeof(v->input_rows[0]), input_kind_text(kind), ": not paired", NULL);
        else if (s->input_dev_confirm == kind)
            COPY_TEXT(v->input_rows[kind - 1u], "TAP AGAIN TO FORGET");
        else
            join_text(row, sizeof(v->input_rows[0]), input_kind_text(kind), ": ", name[0] ? name : "paired");
    }
    COPY_TEXT(v->input_rows[KVM_DISPLAY_INPUT_ADD_ROW], "ADD KEYBOARD OR MOUSE");
    if (s->input_dev_confirm == KVM_DISPLAY_INPUT_KEYBOARD || s->input_dev_confirm == KVM_DISPLAY_INPUT_MOUSE) {
        join_text(v->title, sizeof(v->title), "FORGET ", input_dev_label(s, s->input_dev_confirm), "?");
        COPY_TEXT(v->footer, "Tap again within 5s");
    } else if (s->input_dev_notice == KVM_DISPLAY_INPUT_NOTICE_FORGOTTEN) {
        snprintf(v->footer, sizeof(v->footer), "%s FORGOTTEN", input_kind_text(s->input_dev_notice_kind));
    } else if (s->input_dev_notice == KVM_DISPLAY_INPUT_NOTICE_FAILED) {
        COPY_TEXT(v->footer, "FORGET FAILED, RETRY");
    }
}

static void make_input_view(const kvm_display_status_t *s, uint64_t now_ms,
                            kvm_display_view_t *v)
{
    static const char *const failure[7][2] = {
        {"", ""},
        {"NOT SUPPORTED", "Device is not supported"},
        {"TIMED OUT", "No device paired in time"},
        {"PAIRING FAILED", "Retry and check the code"},
        {"BUSY", "Pairing is busy, try again"},
        {"NO FREE SLOT", "Forget a keyboard or mouse"},
        {"CANCELLED", "Stopping pairing"},
    };
    unsigned long long remaining = s->input_pair_deadline_ms > now_ms
        ? (unsigned long long)((s->input_pair_deadline_ms - now_ms + 999u) / 1000u) : 0u;
    v->screen = KVM_DISPLAY_INPUT_PAIR;
    v->input_cancel = s->touch_available;
    COPY_TEXT(v->title, "ADD KEYBOARD/MOUSE");
    switch (s->input_pair_state) {
    case KVM_DISPLAY_INPUT_DEVICES:
        make_devices_view(s, v);
        break;
    case KVM_DISPLAY_INPUT_SCANNING:
        snprintf(v->footer, sizeof(v->footer), "%llus remaining", remaining);
        if (s->input_pair_connecting) {
            COPY_TEXT(v->primary, "CONNECTING");
            COPY_TEXT(v->detail, s->input_pair_name[0] ? s->input_pair_name : "Input device");
        } else if (!s->input_pair_count) {
            COPY_TEXT(v->primary, "SEARCHING");
            COPY_TEXT(v->detail, "Set device to pairing mode");
        } else {
            v->input_row_count = s->input_pair_count > 4u ? 4u : s->input_pair_count;
            for (uint8_t i = 0; i < v->input_row_count; ++i)
                COPY_TEXT(v->input_rows[i], s->input_pair_rows[i]);
        }
        break;
    case KVM_DISPLAY_INPUT_PASSKEY:
        COPY_TEXT(v->title, "TYPE ON KEYBOARD");
        if (s->input_pair_passkey <= 999999u)
            snprintf(v->primary, sizeof(v->primary), "%06u", (unsigned)s->input_pair_passkey);
        COPY_TEXT(v->detail, "Then press Enter");
        snprintf(v->footer, sizeof(v->footer), "%llus remaining", remaining);
        break;
    default: /* RESULT */
        if (s->input_pair_reason == 0) {
            snprintf(v->primary, sizeof(v->primary), "%s PAIRED", input_kind_text(s->input_pair_kind));
            COPY_TEXT(v->detail, s->input_pair_name[0] ? s->input_pair_name : "Ready to use");
        } else {
            uint8_t reason = s->input_pair_reason <= KVM_DISPLAY_INPUT_REASON_CANCELLED
                                 ? s->input_pair_reason : KVM_DISPLAY_INPUT_REASON_AUTH_FAILED;
            COPY_TEXT(v->primary, failure[reason][0]);
            COPY_TEXT(v->detail, failure[reason][1]);
            if (reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
                s->input_pair_busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST)
                COPY_TEXT(v->detail, "PC is controlling a guest");
            else if (reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
                     s->input_pair_busy_cause == KVM_DISPLAY_INPUT_BUSY_GUEST)
                COPY_TEXT(v->detail, "Close guest pairing first");
        }
        /* Not reported closed until the central confirms (or its timer ran out). */
        if (s->input_pair_stopping) COPY_TEXT(v->footer, "Stopping pairing...");
        break;
    }
    if (s->input_pair_guest_busy) {
        /* Never over the passkey instructions: there the hint takes the footer. */
        if (s->input_pair_state == KVM_DISPLAY_INPUT_PASSKEY) COPY_TEXT(v->footer, "GUEST PAIRING BUSY");
        else COPY_TEXT(v->title, "GUEST PAIRING BUSY");
    }
}

void kvm_display_make_view(const kvm_display_status_t *s, uint64_t now_ms,
                           kvm_display_view_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    if (!s) return;
    v->touch_available = s->touch_available;
    COPY_TEXT(v->card_labels[0], "This PC");
    v->card_visible_mask = 1u;
    v->card_ready_mask = 1u;
    v->selected_card = s->selected_slot <= 3 ? s->selected_slot : 0;
    v->show_guest_settings = s->paired_guests > 0 && s->touch_available &&
                             s->input_pair_state == KVM_DISPLAY_INPUT_CLOSED;
    v->show_input_entry = input_entry_available(s);
    for (uint8_t i = 0; i < 3; ++i) {
        if (!(s->occupied_mask & (1u << i))) continue;
        v->card_visible_mask |= (uint8_t)(1u << (i + 1u));
        if (s->ready_mask & (1u << i)) v->card_ready_mask |= (uint8_t)(1u << (i + 1u));
        if (s->guest_names[i][0]) COPY_TEXT(v->card_labels[i + 1u], s->guest_names[i]);
        else snprintf(v->card_labels[i + 1u], sizeof(v->card_labels[i + 1u]),
                      "Guest %u", i + 1u);
    }
    bool pairing_expired = (s->pairing_state == KVM_DISPLAY_PAIRING_WAITING ||
                            s->pairing_state == KVM_DISPLAY_PAIRING_CHALLENGE) &&
                           s->pairing_deadline_ms <= now_ms;
    bool pairing_terminal = s->pairing_state == KVM_DISPLAY_PAIRING_REJECTED ||
                            s->pairing_state == KVM_DISPLAY_PAIRING_TIMEOUT || pairing_expired;
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
    } else if (input_screen_shown(s)) {
        make_input_view(s, now_ms, v);
    } else if (s->pairing_state != KVM_DISPLAY_PAIRING_CLOSED &&
               !(s->ready_slots && pairing_terminal)) {
        v->screen = KVM_DISPLAY_PAIRING;
        COPY_TEXT(v->title, "PAIR GUEST");
        if (s->pairing_state == KVM_DISPLAY_PAIRING_CHALLENGE &&
            s->pairing_challenge_id && s->pairing_number <= 999999u &&
            s->pairing_deadline_ms > now_ms) {
            snprintf(v->primary, sizeof(v->primary), "%06u", (unsigned)s->pairing_number);
            snprintf(v->detail, sizeof(v->detail), "%llus remaining",
                     (unsigned long long)((s->pairing_deadline_ms - now_ms + 999u) / 1000u));
            COPY_TEXT(v->footer, "Confirm matching code on guest");
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
    } else if (s->usb_connected && s->show_guest_list) {
        v->screen = KVM_DISPLAY_GUEST_LIST;
        COPY_TEXT(v->title, "GUESTS");
        COPY_TEXT(v->primary, "SELECT TARGET");
        v->selectable_slots = s->guest_slots > 3 ? 3 : s->guest_slots;
        for (uint8_t i = 0; i < v->selectable_slots; ++i) {
            const char *state = !(s->ready_mask & (1u << i)) ? "OFFLINE" :
                                (s->armed && s->selected_slot == i + 1u) ? "ACTIVE" : "STANDBY";
            snprintf(v->rows[i], sizeof(v->rows[i]), "GUEST %u  %s", i + 1u, state);
        }
        COPY_TEXT(v->footer, "Use host to select");
    } else {
        v->screen = KVM_DISPLAY_ACTIVE;
        COPY_TEXT(v->title, "KEYBOARD CONTROL");
        COPY_TEXT(v->primary, kvm_display_target_label(s));
        if (s->ready_slots)
            snprintf(v->detail, sizeof(v->detail), "%u guest%s connected",
                     s->ready_slots, s->ready_slots == 1 ? "" : "s");
        else COPY_TEXT(v->detail, "No guest connected");
        COPY_TEXT(v->footer, "Selected target is highlighted");
        /* Highlight rule: with a host route armed the host's selected slot is
           the live target; otherwise (disarmed, or no host session) the device
           input hub target is. A target whose card is not shown falls back to
           This PC so something is always highlighted. */
        uint8_t target = (s->usb_connected && s->armed) ? s->selected_slot : s->hub_target;
        v->selected_card = (target <= 3u && (v->card_visible_mask & (1u << target))) ? target : 0;
        /* No host session is not a pause: the same view, dimmed, with a badge.
           Only usb_dim and the footer differ from the session view, so toggling
           the session never changes the screen. Fault, recovery, update and
           pairing were handled above and keep their precedence. */
        if (!s->usb_connected) {
            v->usb_dim = true;
            COPY_TEXT(v->footer, "USB off");
        }
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
    /* No guest pairing action while the input-device window or its result shows. */
    if (s->input_pair_state != KVM_DISPLAY_INPUT_CLOSED) return false;
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
        if (s->pairing_state != KVM_DISPLAY_PAIRING_CHALLENGE ||
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
        s->updating || s->recovery_required) return false;
    if (s->input_pair_state != KVM_DISPLAY_INPUT_CLOSED) return false;
    switch (s->pairing_state) {
    case KVM_DISPLAY_PAIRING_CLOSED:
    case KVM_DISPLAY_PAIRING_REJECTED:
    case KVM_DISPLAY_PAIRING_TIMEOUT:
        if (s->paired_guests) {
            if (x < 190u || x >= 232u || y < 194u || y >= 236u) return false;
        } else if (x < 12u || x >= 228u || y < 160u || y >= 206u) return false;
        *action = KVM_DISPLAY_PAIR_BEGIN; return true;
    case KVM_DISPLAY_PAIRING_WAITING:
        if (!s->pairing_local_owner || x < 12u || x >= 115u ||
            y < 160u || y >= 206u) return false;
        *action = KVM_DISPLAY_PAIR_CANCEL; return true;
    case KVM_DISPLAY_PAIRING_CHALLENGE:
        return false;
    default: return false;
    }
}

bool kvm_display_input_touch_action(const kvm_display_status_t *s,
                                    uint16_t x, uint16_t y,
                                    kvm_display_input_action_t *action, uint8_t *row)
{
    if (!s || !action || !row) return false;
    if (s->input_pair_state == KVM_DISPLAY_INPUT_CLOSED) {
        if (!input_entry_available(s) ||
            x < KVM_DISPLAY_INPUT_ENTRY_X || x >= KVM_DISPLAY_INPUT_ENTRY_X + KVM_DISPLAY_INPUT_ENTRY_SIZE ||
            y < KVM_DISPLAY_INPUT_ENTRY_Y || y >= KVM_DISPLAY_INPUT_ENTRY_Y + KVM_DISPLAY_INPUT_ENTRY_SIZE)
            return false;
        *action = KVM_DISPLAY_INPUT_DEVICES_OPEN;
        *row = 0;
        return true;
    }
    if (!s->touch_available || !input_screen_shown(s)) return false;
    if (x >= KVM_DISPLAY_INPUT_CANCEL_X && x < KVM_DISPLAY_INPUT_CANCEL_X + KVM_DISPLAY_INPUT_CANCEL_W &&
        y < KVM_DISPLAY_INPUT_CANCEL_H) {
        *action = KVM_DISPLAY_INPUT_PAIR_CANCEL;
        *row = 0;
        return true;
    }
    if (s->input_pair_state == KVM_DISPLAY_INPUT_DEVICES) {
        if (x < KVM_DISPLAY_INPUT_ROW_X || x >= KVM_DISPLAY_INPUT_ROW_X + KVM_DISPLAY_INPUT_ROW_W ||
            y < KVM_DISPLAY_INPUT_ROW_Y) return false;
        uint32_t index = (uint32_t)(y - KVM_DISPLAY_INPUT_ROW_Y) / KVM_DISPLAY_INPUT_ROW_H;
        if (index >= KVM_DISPLAY_INPUT_DEVICE_ROWS) return false;
        *action = index == KVM_DISPLAY_INPUT_ADD_ROW ? KVM_DISPLAY_INPUT_PAIR_BEGIN : KVM_DISPLAY_INPUT_FORGET;
        *row = index == KVM_DISPLAY_INPUT_ADD_ROW ? 0 : (uint8_t)index;
        return true;
    }
    if (s->input_pair_state != KVM_DISPLAY_INPUT_SCANNING || s->input_pair_connecting ||
        x < KVM_DISPLAY_INPUT_ROW_X || x >= KVM_DISPLAY_INPUT_ROW_X + KVM_DISPLAY_INPUT_ROW_W ||
        y < KVM_DISPLAY_INPUT_ROW_Y) return false;
    uint32_t index = (uint32_t)(y - KVM_DISPLAY_INPUT_ROW_Y) / KVM_DISPLAY_INPUT_ROW_H;
    uint8_t count = s->input_pair_count > 4u ? 4u : s->input_pair_count;
    if (index >= count) return false;
    *action = KVM_DISPLAY_INPUT_PAIR_SELECT;
    *row = (uint8_t)index;
    return true;
}

bool kvm_display_input_request(const kvm_display_status_t *s,
                               kvm_display_input_action_t action, uint8_t row,
                               kvm_display_input_request_t *request)
{
    if (!request) return false;
    memset(request, 0, sizeof(*request));
    if (!s || !s->touch_available) return false;
    switch (action) {
    case KVM_DISPLAY_INPUT_DEVICES_OPEN:
        if (!input_entry_available(s)) return false;
        break;
    case KVM_DISPLAY_INPUT_PAIR_BEGIN:
        if (!input_entry_available(s) && !input_list_usable(s)) return false;
        break;
    case KVM_DISPLAY_INPUT_FORGET:
        if (!input_list_usable(s) || row >= 2u || !input_dev_stored(s, (uint8_t)(row + 1u))) return false;
        request->row = row;
        break;
    case KVM_DISPLAY_INPUT_PAIR_SELECT:
        if (!input_screen_shown(s) || s->input_pair_state != KVM_DISPLAY_INPUT_SCANNING ||
            s->input_pair_connecting || row >= 4u || row >= s->input_pair_count) return false;
        request->row = row;
        request->scan_index = s->input_pair_index[row];
        break;
    case KVM_DISPLAY_INPUT_PAIR_CANCEL:
        if (s->input_pair_state == KVM_DISPLAY_INPUT_CLOSED) return false;
        break;
    default: return false;
    }
    request->action = action;
    request->window = s->input_pair_window;
    return true;
}

uint8_t kvm_display_touch_slot(const kvm_display_view_t *v, uint16_t y)
{
    if (!v || !v->touch_available || v->screen != KVM_DISPLAY_GUEST_LIST ||
        y < 44u || y >= 176u) return 0;
    uint8_t index = (uint8_t)((y - 44u) / 44u);
    if (index >= v->selectable_slots || strstr(v->rows[index], "OFFLINE")) return 0;
    return index + 1u;
}

uint8_t kvm_display_next_ready_slot(uint8_t ready_mask, uint8_t current_slot)
{
    for (uint8_t slot = current_slot + 1u; slot <= 3u; ++slot)
        if (ready_mask & (1u << (slot - 1u))) return slot;
    return 0;
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

void kvm_display_badge_text(const kvm_display_status_t *status, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!status) return;
    snprintf(out, n, "%s%s%s%s", status->usb_connected ? "USB on" : "USB off",
             status->input_kbd || status->input_mouse ? " |" : "",
             status->input_kbd ? " KB" : "", status->input_mouse ? " MS" : "");
}
