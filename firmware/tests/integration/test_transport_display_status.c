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

/* USB mode facts for the screen and the worker's handling of its requests. */
static void usb_mode_tests(void)
{
    kvm_transport_core_t core;
    kvm_router_t router = {0};
    kvm_display_status_t status;
    uint64_t now = 1000;
    assert(kvm_transport_core_init(&core, "esp32-kvm-s3", "test", 9, discard, NULL));
    kvm_transport_core_bind_router(&core, &router, test_now, &now);
    core.session_open = true; router.session_open = true;

    /* The base projection leaves the USB fields at 0, which is today's screen. */
    memset(&status, 0xA5, sizeof status);
    kvm_transport_display_status(&core, &router, true, false, now, &status);
    assert(status.usb_mode == 0 && status.pc_hid == 0 && status.usb_reason == 0);
    assert(status.usb_result == 0 && !status.usb_screen && status.usb_confirm == 0 &&
           status.usb_notice == 0);
    memset(&status, 0xA5, sizeof status);
    kvm_transport_display_status(&core, &router, false, false, now, &status);
    assert(status.usb_mode == 0 && status.pc_hid == 0 && status.usb_reason == 0);

    /* The boot facts are copied through, after the base projection. */
    kvm_transport_display_status(&core, &router, true, false, now, &status);
    kvm_transport_display_usb(1, 1, 2, &status);
    assert(status.usb_mode == 1 && status.usb_reason == 1 && status.pc_hid == 2);
    assert(status.usb_connected); /* nothing else touched */
    kvm_transport_display_usb(1, 7, 1, &status);
    assert(status.usb_mode == 1 && status.usb_reason == 7 && status.pc_hid == 1);
    kvm_transport_display_usb(1, 1, 0, &status);
    assert(status.pc_hid == 0);
    /* Out of range PC state reads as "not enumerated". */
    kvm_transport_display_usb(1, 1, 9, &status);
    assert(status.usb_mode == 1 && status.pc_hid == 0);
    /* Serial mode has no PC HID, whatever is passed; the reason still shows. */
    kvm_transport_display_usb(0, 2, 2, &status);
    assert(status.usb_mode == 0 && status.pc_hid == 0 && status.usb_reason == 2);
    kvm_transport_display_usb(5, 4, 2, &status);
    assert(status.usb_mode == 0 && status.pc_hid == 0 && status.usb_reason == 4);
    kvm_transport_display_usb(0, 0, 0, NULL);

    /* Worker request queue: bounded, FIFO, a repeat replaces its older copy. */
    kvm_transport_usb_queue_t q;
    kvm_display_usb_request_t r = (kvm_display_usb_request_t)0;
    memset(&q, 0, sizeof q);
    assert(!kvm_transport_usb_queue_pop(&q, &r));
    assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_USE_SERIAL));
    assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_SERIAL_ONCE));
    assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_USE_SERIAL)); /* repeat: moves last */
    assert(q.count == 2);
    assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_USE_COMPOSITE));
    for (int i = 0; i < 10; ++i) assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_USE_COMPOSITE));
    assert(q.count == 3 && q.count <= KVM_TRANSPORT_USB_QUEUE_DEPTH);
    assert(kvm_transport_usb_queue_pop(&q, &r) && r == KVM_DISPLAY_USB_SERIAL_ONCE);
    assert(kvm_transport_usb_queue_pop(&q, &r) && r == KVM_DISPLAY_USB_USE_SERIAL);
    assert(kvm_transport_usb_queue_pop(&q, &r) && r == KVM_DISPLAY_USB_USE_COMPOSITE);
    assert(!kvm_transport_usb_queue_pop(&q, &r) && q.count == 0);
    /* Values outside the enum are refused; NULL is safe. */
    assert(!kvm_transport_usb_queue_push(&q, (kvm_display_usb_request_t)0));
    assert(!kvm_transport_usb_queue_push(&q, (kvm_display_usb_request_t)4));
    assert(q.count == 0);
    assert(!kvm_transport_usb_queue_push(NULL, KVM_DISPLAY_USB_USE_SERIAL));
    assert(!kvm_transport_usb_queue_pop(NULL, &r));
    assert(kvm_transport_usb_queue_push(&q, KVM_DISPLAY_USB_USE_SERIAL));
    assert(!kvm_transport_usb_queue_pop(&q, NULL) && q.count == 1);

    /* Worker action: composite is refused without support (nothing released,
       nothing written); the restarts only when no guest is armed and no guest
       pairing window is open. */
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_COMPOSITE, false, false, false) ==
           KVM_TRANSPORT_USB_ACTION_NOT_BUILT);
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_COMPOSITE, false, true, true) ==
           KVM_TRANSPORT_USB_ACTION_NOT_BUILT);
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_COMPOSITE, true, false, false) ==
           KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE);
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_SERIAL, false, false, false) ==
           KVM_TRANSPORT_USB_ACTION_SERIAL_MODE);
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_SERIAL, true, false, false) ==
           KVM_TRANSPORT_USB_ACTION_SERIAL_MODE);
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_SERIAL_ONCE, false, false, false) ==
           KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE);
    for (int c = 0; c < 2; ++c)
    for (int g = 0; g < 3; ++g) {
        bool armed = g != 1, guest = g != 0;
        assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_SERIAL, c != 0, armed, guest) ==
               KVM_TRANSPORT_USB_ACTION_BUSY);
        assert(kvm_transport_usb_action(KVM_DISPLAY_USB_SERIAL_ONCE, c != 0, armed, guest) ==
               KVM_TRANSPORT_USB_ACTION_BUSY);
    }
    assert(kvm_transport_usb_action(KVM_DISPLAY_USB_USE_COMPOSITE, true, true, false) ==
           KVM_TRANSPORT_USB_ACTION_BUSY);
    assert(kvm_transport_usb_action((kvm_display_usb_request_t)0, true, false, false) ==
           KVM_TRANSPORT_USB_ACTION_NONE);
    assert(kvm_transport_usb_action((kvm_display_usb_request_t)9, true, false, false) ==
           KVM_TRANSPORT_USB_ACTION_NONE);
    /* The worker's result for each action, as the screen shows it. */
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_NONE) == KVM_DISPLAY_USB_RESULT_NONE);
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_NOT_BUILT) == KVM_DISPLAY_USB_RESULT_NOT_BUILT);
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_BUSY) == KVM_DISPLAY_USB_RESULT_BUSY);
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE) == KVM_DISPLAY_USB_RESULT_RESTARTING);
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_SERIAL_MODE) == KVM_DISPLAY_USB_RESULT_RESTARTING);
    assert(kvm_transport_usb_action_result(KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE) == KVM_DISPLAY_USB_RESULT_RESTARTING);
    assert(kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE) &&
           kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_SERIAL_MODE) &&
           kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE));
    assert(!kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_NONE) &&
           !kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_NOT_BUILT) &&
           !kvm_transport_usb_action_reboots(KVM_TRANSPORT_USB_ACTION_BUSY));

    /* A worker result reaches the screen's footer through the snapshot. */
    {
        kvm_display_view_t view;
        kvm_display_usb_ui_t ui;
        kvm_display_usb_ui_init(&ui);
        ui.open = true; ui.idle_until_ms = now + 10000;
        kvm_transport_display_status(&core, &router, true, false, now, &status);
        kvm_transport_display_usb(0, 2, 0, &status);
        status.usb_result = KVM_DISPLAY_USB_RESULT_NOT_BUILT;
        status.touch_available = true;
        kvm_display_usb_tick(&ui, &status, now);
        kvm_display_usb_status(&ui, now, &status);
        kvm_display_make_view(&status, now, &view);
        assert(view.screen == KVM_DISPLAY_USB_MODE);
        assert(strcmp(view.title, "USB MODE: SERIAL") == 0);
        assert(strcmp(view.detail, "Not in this build") == 0);
        assert(strcmp(view.footer, "Not in this build") == 0);
    }
}

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
    assert(view.screen == KVM_DISPLAY_ACTIVE && view.usb_dim);
    assert(strcmp(view.detail, "No guest connected") == 0);

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

    usb_mode_tests();
    return 0;
}
