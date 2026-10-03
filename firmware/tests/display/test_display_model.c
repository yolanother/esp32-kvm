/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks confirmed device screen priority, guest row eligibility, numeric
 * pairing expiry, and deterministic PLUS/BOOT button behavior. */
#include "display_model.h"
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
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE);
    assert(strcmp(view.primary, "GUEST 1") == 0);
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
    status.usb_connected = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAUSED);
    assert(strcmp(view.primary, "INPUT PAUSED") == 0);
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
    status.show_guest_list = false;
    status.pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
    status.pairing_challenge_id = 7;
    status.pairing_number = 123;
    status.pairing_deadline_ms = 61000;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "000123") == 0);
    assert(strcmp(view.detail, "60s remaining") == 0);
    kvm_display_make_view(&status, 61000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_challenge_id = 0;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_challenge_id = 7;
    status.pairing_number = 1000000;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
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
    status.recovery_required = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_RECOVERY);
    status.recovery_required = false;
    status.updating = true;
    status.update_percent = 42;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_UPDATE);
    assert(strcmp(view.primary, "42%") == 0);

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
    return 0;
}
