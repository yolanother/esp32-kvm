/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks disarmed labels and deterministic PLUS/BOOT debounce and long-press
 * behavior, including the BOOT-at-power-on recovery exclusion. */
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
    assert(strcmp(kvm_display_target_label(&status), "GUEST 1") == 0);
    status.fault = true;
    assert(strcmp(kvm_display_target_label(&status), "LOCAL ERROR") == 0);

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
