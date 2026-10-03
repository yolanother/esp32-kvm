/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Proves serialized transport, router, and authenticated HID facts become
 * fail-closed device screen status without inventing an update or touch source. */
#include "transport_display_status.h"
#include "display_model.h"
#include <assert.h>
#include <string.h>

static void discard(void *context, const uint8_t *bytes, size_t length)
{ (void)context; (void)bytes; (void)length; }

static uint64_t test_now(void *context)
{ return *(const uint64_t *)context; }

int main(void)
{
    uint64_t now = 1000;
    kvm_transport_core_t core;
    kvm_router_t router = {0};
    kvm_display_status_t status;
    kvm_display_view_t view;
    uint8_t token[16] = {1};
    assert(kvm_transport_core_init(&core, "esp32-kvm-s3", "test", 7, discard, NULL));
    kvm_transport_core_bind_router(&core, &router, test_now, &now);
    core.session_open = true;
    core.minor = 2;
    router.session_open = true;
    router.generation = 3;
    router.slot = 1;
    router.armed = true;
    kvm_transport_core_set_connected_token(&core, token);
    kvm_transport_core_pairing_event(&core, KVM_PAIRING_CHALLENGE, 9, 123, 61000);

    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.usb_connected && status.armed && status.guest_ready);
    assert(status.guest_slots == 1 && status.ready_slots == 1 && status.ready_mask == 1);
    assert(status.selected_slot == 1 && status.generation == 3);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_CHALLENGE);
    assert(status.pairing_challenge_id == 9 && status.pairing_number == 123);
    assert(status.pairing_deadline_ms == 61000);
    assert(!status.updating && !status.recovery_required && !status.touch_available);
    kvm_display_make_view(&status, now, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "000123") == 0);

    now = 61000;
    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_TIMEOUT);
    assert(status.pairing_challenge_id == 0 && status.pairing_number == 0);
    kvm_display_make_view(&status, now, &view);
    assert(strcmp(view.primary, "GUEST 1") == 0);

    kvm_transport_core_pairing_event(&core, KVM_PAIRING_WAITING, 0, 0, now + 5000);
    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_WAITING);
    assert(status.pairing_deadline_ms == now + 5000);
    assert(status.pairing_challenge_id == 0 && status.pairing_number == 0);
    kvm_transport_core_pairing_event(&core, KVM_PAIRING_CAPACITY, 0, 0, 0);
    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_CAPACITY);
    kvm_transport_core_pairing_event(&core, KVM_PAIRING_REJECTED, 0, 0, 0);
    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_REJECTED);

    kvm_transport_core_set_connected_token(&core, NULL);
    kvm_transport_display_status(&core, &router, true, true, now, &status);
    assert(status.guest_slots == 0 && status.ready_slots == 0 && status.ready_mask == 0);
    assert(!status.armed && !status.guest_ready && status.fault);

    kvm_transport_display_status(&core, &router, false, true, now, &status);
    assert(!status.usb_connected && !status.armed && !status.fault);
    assert(status.pairing_state == KVM_DISPLAY_PAIRING_CLOSED);
    assert(status.pairing_challenge_id == 0 && status.pairing_deadline_ms == 0);
    kvm_display_make_view(&status, now, &view);
    assert(view.screen == KVM_DISPLAY_PAUSED);

    kvm_transport_core_reset(&core);
    kvm_transport_display_status(&core, &router, true, false, now, &status);
    assert(!status.armed && status.selected_slot == 0 && status.guest_slots == 0);
    assert(kvm_transport_core_init(&core, "esp32-kvm-s3", "test", 8, discard, NULL));
    core.session_open = true; core.minor = 2;
    router.session_open = true; router.slot = 3; router.armed = true; router.fault = false;
    assert(kvm_router_set_capacity(&router, 3) == false); /* Active route cannot change capacity. */
    router.capacity = 3;
    kvm_transport_slot_t slots[3] = {0};
    for (uint8_t i = 0; i < 3; ++i) {
        slots[i].token[0] = (uint8_t)(i + 1);
        slots[i].ready = slots[i].subscribed = true;
    }
    assert(kvm_transport_core_set_slots(&core, slots));
    kvm_transport_display_status(&core, &router, true, false, now, &status);
    assert(status.guest_slots == 3 && status.ready_slots == 3 && status.ready_mask == 7);
    assert(status.selected_slot == 3 && status.guest_ready && status.armed && !status.fault);
    memset(&slots[0], 0, sizeof(slots[0]));
    memset(&slots[2], 0, sizeof(slots[2]));
    router.slot = 2;
    assert(kvm_transport_core_set_slots(&core, slots));
    kvm_transport_display_status(&core, &router, true, false, now, &status);
    assert(status.guest_slots == 2 && status.ready_slots == 1 && status.ready_mask == 2);
    assert(status.selected_slot == 2 && status.guest_ready && status.armed && !status.fault);
    return 0;
}
