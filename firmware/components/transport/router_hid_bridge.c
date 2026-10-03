/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Maps each selected route slot to its bounded NimBLE-host command wrapper.
 * A release failure requests physical disconnect, preventing uncertain held
 * reports from being reused after local failover. */
#include "router_hid_bridge.h"
#include "hid_guest.h"

static bool ready(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && hid_guest_request_ready_slot(slot);
}

static bool release(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && hid_guest_request_release_slot(slot);
}

static bool arm(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && hid_guest_request_arm_slot(slot);
}

static bool send_input(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    (void)context;
    if (slot < 1 || slot > 3 || !input) return false;
    switch (input->kind) {
    case KVM_ROUTER_KEYBOARD: return hid_guest_request_keyboard_slot(slot, input->keyboard);
    case KVM_ROUTER_POINTER:
        return hid_guest_request_mouse_slot(slot, input->buttons, input->dx, input->dy,
                                            input->wheel, input->pan);
    case KVM_ROUTER_CONSUMER: return hid_guest_request_consumer_slot(slot, input->consumer);
    default: return false;
    }
}

static void disconnect_guest(void *context, uint8_t slot)
{
    (void)context;
    if (slot >= 1 && slot <= 3) (void)hid_guest_request_disconnect_slot(slot);
}

kvm_router_output_t kvm_router_hid_output(void)
{
    kvm_router_output_t output = {ready, release, arm, send_input, disconnect_guest};
    return output;
}
