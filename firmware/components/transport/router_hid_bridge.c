/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Maps the router's one selected guest to bounded NimBLE-host command wrappers.
 * A release failure requests physical disconnect, preventing uncertain held
 * reports from being reused after local failover. */
#include "router_hid_bridge.h"
#include "hid_guest.h"

static bool ready(void *context, uint8_t slot)
{
    (void)context;
    return slot == 1 && hid_guest_request_ready();
}

static bool release(void *context, uint8_t slot)
{
    (void)context;
    return slot == 1 && hid_guest_request_release();
}

static bool arm(void *context, uint8_t slot)
{
    (void)context;
    return slot == 1 && hid_guest_request_arm();
}

static bool send_input(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    (void)context;
    if (slot != 1 || !input) return false;
    switch (input->kind) {
    case KVM_ROUTER_KEYBOARD: return hid_guest_request_keyboard(input->keyboard);
    case KVM_ROUTER_POINTER:
        return hid_guest_request_mouse(input->buttons, input->dx, input->dy,
                                       input->wheel, input->pan);
    case KVM_ROUTER_CONSUMER: return hid_guest_request_consumer(input->consumer);
    default: return false;
    }
}

static void disconnect_guest(void *context, uint8_t slot)
{
    (void)context;
    if (slot == 1) (void)hid_guest_request_disconnect();
}

kvm_router_output_t kvm_router_hid_output(void)
{
    kvm_router_output_t output = {ready, release, arm, send_input, disconnect_guest};
    return output;
}
