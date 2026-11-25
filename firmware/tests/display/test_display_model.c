/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks confirmed device screen priority, guest row eligibility, numeric
 * pairing expiry, and deterministic PLUS/BOOT button behavior. */
#include "display_model.h"
#include "display_pairing.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    kvm_display_status_t status = {0};
    assert(strcmp(kvm_display_target_label(&status), "HOST") == 0);
    status.selected_slot = 1;
    assert(strcmp(kvm_display_target_label(&status), "DISARMED") == 0);
    status.armed = true;
    assert(strcmp(kvm_display_target_label(&status), "LOCAL ERROR") == 0);
    status.guest_ready = true;
    assert(strcmp(kvm_display_target_label(&status), "GUEST 1") == 0);
    status.fault = true;
    assert(strcmp(kvm_display_target_label(&status), "LOCAL ERROR") == 0);

    kvm_display_view_t view;
    status.fault = false;
    status.usb_connected = true;
    status.guest_slots = 2;
    status.ready_slots = 1;
    status.ready_mask = 1;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE);
    assert(strcmp(view.primary, "GUEST 1") == 0);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    status.guest_ready = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAUSED);
    assert(strcmp(view.primary, "INPUT PAUSED") == 0);
    assert(strcmp(view.title, "ROUTING FAULT") == 0);
    assert(strcmp(view.footer, "Use host to release") == 0);
    status.guest_ready = true;
    status.armed = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "DISARMED") == 0);
    /* usb_off_with_ready_guest_shows_active_dimmed */
    status.usb_connected = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE);
    assert(view.usb_dim);
    assert(strcmp(view.title, "KEYBOARD CONTROL") == 0);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    assert(strcmp(view.footer, "USB off") == 0);
    assert(strcmp(view.primary, "DISARMED") == 0);
    status.usb_connected = true;
    status.show_guest_list = true;
    status.touch_available = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_GUEST_LIST);
    assert(strcmp(view.rows[0], "GUEST 1  STANDBY") == 0);
    assert(strcmp(view.rows[1], "GUEST 2  OFFLINE") == 0);
    assert(kvm_display_touch_slot(&view, 64) == 1);
    assert(kvm_display_touch_slot(&view, 110) == 0);
    assert(kvm_display_touch_slot(&view, 240) == 0);
    status.ready_mask = 2;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.rows[0], "GUEST 1  OFFLINE") == 0);
    assert(strcmp(view.rows[1], "GUEST 2  STANDBY") == 0);
    assert(kvm_display_touch_slot(&view, 64) == 0);
    assert(kvm_display_touch_slot(&view, 110) == 2);
    assert(kvm_display_next_ready_slot(0b110, 0) == 2);
    assert(kvm_display_next_ready_slot(0b110, 2) == 3);
    assert(kvm_display_next_ready_slot(0b110, 3) == 0);
    status.show_guest_list = false;
    status.pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
    status.pairing_local_owner = true;
    status.pairing_challenge_id = 7;
    status.pairing_number = 123;
    status.pairing_deadline_ms = 61000;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "000123") == 0);
    assert(strcmp(view.detail, "60s remaining") == 0);
    status.ready_slots = 0;
    kvm_display_make_view(&status, 61000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.ready_slots = 1;
    status.usb_connected = false;
    kvm_display_make_view(&status, 61000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE && view.usb_dim);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    status.usb_connected = true;
    status.ready_slots = 1;
    status.pairing_challenge_id = 0;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_challenge_id = 7;
    status.pairing_number = 1000000;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.ready_slots = 0;
    status.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
    status.pairing_deadline_ms = 6100;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "WAITING FOR GUEST") == 0);
    assert(strcmp(view.detail, "6s remaining") == 0);
    kvm_display_make_view(&status, 6100, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_deadline_ms = 0;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.usb_connected = false;
    status.pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
    status.pairing_challenge_id = 91;
    status.pairing_number = 123456;
    status.pairing_deadline_ms = 61000;
    status.touch_available = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "123456") == 0);
    kvm_display_pair_request_t request;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 91);
    kvm_display_pair_action_t touched;
    assert(strcmp(view.footer, "Confirm matching code on guest") == 0);
    assert(!kvm_display_pair_touch_action(&status, 40, 180, &touched));
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    assert(!kvm_display_pair_touch_action(&status, 180, 120, &touched));
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_REJECT, &request));
    assert(request.challenge_id == 91);
    status.pairing_local_owner = false;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 91);
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    status.pairing_local_owner = true;
    assert(!kvm_display_pair_request(&status, 61000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 0);
    status.touch_available = false;
    assert(!kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    status.touch_available = true;
    status.pairing_state = KVM_DISPLAY_PAIRING_CLOSED;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_BEGIN, &request));
    assert(request.challenge_id == 0);
    assert(kvm_display_pair_touch_action(&status, 120, 180, &touched));
    assert(touched == KVM_DISPLAY_PAIR_BEGIN);
    status.paired_guests = 2;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.show_guest_settings);
    assert(!kvm_display_pair_touch_action(&status, 120, 180, &touched));
    assert(kvm_display_pair_touch_action(&status, 210, 214, &touched));
    assert(touched == KVM_DISPLAY_PAIR_BEGIN);
    status.paired_guests = 0;
    status.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_CANCEL, &request));
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    assert(kvm_display_pair_touch_action(&status, 40, 180, &touched));
    assert(touched == KVM_DISPLAY_PAIR_CANCEL);
    status.pairing_state = KVM_DISPLAY_PAIRING_CAPACITY;
    assert(!kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_BEGIN, &request));
    status.pairing_state = KVM_DISPLAY_PAIRING_CLOSED;
    status.usb_connected = true;
    status.recovery_required = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_RECOVERY);
    status.recovery_required = false;
    status.updating = true;
    status.update_percent = 42;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_UPDATE);
    assert(strcmp(view.primary, "42%") == 0);

    /* No host session: the guest view stays, only usb_dim/footer change. */
    {
        kvm_display_status_t s = {0};
        kvm_display_view_t on, off;
        /* Toggling the session never changes the screen except usb_dim and
           footer, for every guest count, hub target and selected slot. */
        for (uint8_t ready = 0; ready <= 3; ++ready)
        for (uint8_t hub = 0; hub <= 3; ++hub)
        for (uint8_t sel = 0; sel <= 3; ++sel) {
            memset(&s, 0, sizeof s);
            s.guest_slots = ready; s.ready_slots = ready;
            s.ready_mask = s.occupied_mask = (uint8_t)((1u << ready) - 1u);
            s.guest_ready = true; s.selected_slot = sel; s.hub_target = hub;
            s.usb_connected = s.host_session = true;
            kvm_display_make_view(&s, 5000, &on);
            s.usb_connected = s.host_session = false;
            kvm_display_make_view(&s, 5000, &off);
            assert(on.screen == KVM_DISPLAY_ACTIVE && off.screen == on.screen);
            assert(strcmp(on.title, "KEYBOARD CONTROL") == 0);
            assert(strcmp(off.title, on.title) == 0);
            assert(strcmp(off.primary, on.primary) == 0);
            assert(strcmp(off.detail, on.detail) == 0);
            assert(off.card_visible_mask == on.card_visible_mask);
            assert(off.card_ready_mask == on.card_ready_mask);
            assert(off.selected_card == on.selected_card);
            assert(on.selected_card == (hub <= ready ? hub : 0)); /* unoccupied: This PC */
            assert(!on.usb_dim && off.usb_dim);
            assert(strcmp(off.footer, "USB off") == 0);
            assert(strcmp(on.footer, "Selected target is highlighted") == 0);
            /* Everything else is identical once usb_dim and footer are aligned. */
            off.usb_dim = false;
            memcpy(off.footer, on.footer, sizeof off.footer);
            assert(memcmp(&on, &off, sizeof on) == 0);
        }
        /* usb_off_no_guest_shows_guest_view_not_pause */
        memset(&s, 0, sizeof s);
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_ACTIVE && off.usb_dim);
        assert(strcmp(off.title, "KEYBOARD CONTROL") == 0);
        assert(strcmp(off.detail, "No guest connected") == 0);
        assert(strcmp(off.footer, "USB off") == 0);
        assert(off.card_visible_mask == 1u && off.selected_card == 0);
        /* An armed host route drives the highlight while the session is on. */
        s.usb_connected = s.host_session = true; s.armed = true; s.guest_ready = true;
        s.guest_slots = s.ready_slots = 2; s.ready_mask = s.occupied_mask = 3;
        s.selected_slot = 2; s.hub_target = 1;
        kvm_display_make_view(&s, 5000, &on);
        assert(on.selected_card == 2);
        memset(&s, 0, sizeof s);
        /* fault_still_pauses and armed-without-ready still pause with no session */
        s.fault = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAUSED && strcmp(off.primary, "INPUT PAUSED") == 0);
        assert(!off.usb_dim);
        s.fault = false; s.armed = true; s.guest_ready = false;
        s.ready_slots = 1; s.ready_mask = 1; s.occupied_mask = 1; s.guest_slots = 1;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAUSED && strcmp(off.title, "ROUTING FAULT") == 0);
        /* recovery_still_wins and updating still win */
        s.armed = false; s.recovery_required = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_RECOVERY && !off.usb_dim);
        s.recovery_required = false; s.updating = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_UPDATE);
        s.updating = false;
        /* pairing keeps precedence over the no-session view */
        s.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
        s.pairing_deadline_ms = 9000;
        s.ready_slots = 0; s.ready_mask = 0; s.occupied_mask = 0; s.guest_slots = 0;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAIRING && !off.usb_dim);
        /* show_guest_list unchanged with a host session */
        memset(&s, 0, sizeof s);
        s.usb_connected = s.host_session = true;
        s.guest_slots = 1; s.ready_slots = 1; s.ready_mask = 1; s.occupied_mask = 1;
        s.show_guest_list = true;
        kvm_display_make_view(&s, 5000, &on);
        assert(on.screen == KVM_DISPLAY_GUEST_LIST && !on.usb_dim);
        assert(strcmp(on.rows[0], "GUEST 1  STANDBY") == 0);
        assert(strcmp(on.footer, "Use host to select") == 0);
    }

    /* Footer badge text: USB state first, then connected input devices. */
    {
        kvm_display_status_t s;
        char out[32];
        memset(&s, 0, sizeof s);
        s.input_kbd = s.input_mouse = true;
        kvm_display_badge_text(&s, out, sizeof out);
        assert(strcmp(out, "USB off | KB MS") == 0);  /* badge_text_lists_usb_and_inputs */
        for (unsigned m = 0; m < 8; ++m) {
            static const char *const want[8] = {
                "USB off", "USB off | KB", "USB off | MS", "USB off | KB MS",
                "USB on", "USB on | KB", "USB on | MS", "USB on | KB MS"};
            memset(&s, 0, sizeof s);
            s.usb_connected = (m & 4u) != 0; s.input_kbd = (m & 1u) != 0;
            s.input_mouse = (m & 2u) != 0;
            kvm_display_badge_text(&s, out, sizeof out);
            assert(strcmp(out, want[m]) == 0);
        }
        /* Buffer edges: always terminated, truncated, never overrun. */
        s.usb_connected = s.input_kbd = s.input_mouse = true;
        char guard[8]; memset(guard, 'x', sizeof guard);
        kvm_display_badge_text(&s, guard, 0);
        assert(guard[0] == 'x');
        kvm_display_badge_text(&s, guard, 1);
        assert(guard[0] == '\0' && guard[1] == 'x');
        kvm_display_badge_text(&s, guard, 5);
        assert(strcmp(guard, "USB ") == 0 && guard[5] == 'x');
        memset(guard, 'x', sizeof guard);
        kvm_display_badge_text(NULL, guard, sizeof guard);
        assert(guard[0] == '\0');
        kvm_display_badge_text(&s, NULL, 4);
    }

    kvm_display_buttons_t b;
    kvm_display_buttons_init(&b, false, true, 0);
    assert(kvm_display_buttons_sample(&b, false, true, 2000) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2010) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2045) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, true, false, 2100) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, true, false, 2135) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2200) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2235) == KVM_DISPLAY_NEXT_REQUEST);
    assert(kvm_display_buttons_sample(&b, false, true, 2300) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, true, 2335) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, true, 3335) == KVM_DISPLAY_EMERGENCY_RELEASE);
    assert(kvm_display_buttons_sample(&b, false, true, 4335) == KVM_DISPLAY_NO_EVENT);

    kvm_display_pairing_t pairing = {0};
    kvm_display_pairing_started(&pairing, true, 1000);
    assert(pairing.local_owner && pairing.state == KVM_DISPLAY_PAIRING_WAITING);
    assert(pairing.deadline_ms == 61000);
    assert(kvm_display_pairing_event(&pairing, KVM_DISPLAY_PAIRING_CHALLENGE,
                                     91, 123456, 61000, 2000));
    request = (kvm_display_pair_request_t){KVM_DISPLAY_PAIR_APPROVE, 90};
    assert(!kvm_display_pairing_accept(&pairing, request, 2000));
    request.challenge_id = 91;
    assert(kvm_display_pairing_accept(&pairing, request, 2000));
    pairing.local_owner = false;
    assert(kvm_display_pairing_accept(&pairing, request, 2000));
    assert(!kvm_display_pairing_accept(&pairing, request, 61000));
    kvm_display_pairing_replied(&pairing, true);
    assert(pairing.state == KVM_DISPLAY_PAIRING_WAITING && pairing.challenge_id == 0);
    assert(!kvm_display_pairing_accept(&pairing, request, 2000));
    kvm_display_pairing_expire(&pairing, 61000);
    assert(pairing.state == KVM_DISPLAY_PAIRING_TIMEOUT && pairing.number == 0);
    kvm_display_pairing_clear(&pairing);
    assert(pairing.state == KVM_DISPLAY_PAIRING_CLOSED && !pairing.local_owner);
    return 0;
}
