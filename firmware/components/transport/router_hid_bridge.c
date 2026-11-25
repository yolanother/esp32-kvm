/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Maps each selected route slot to its bounded NimBLE-host command wrapper and
 * copies central input-device reports into hub queue messages.
 * A release failure requests physical disconnect, preventing uncertain held
 * reports from being reused after local failover. */
#include "router_hid_bridge.h"
#include "doubtech_hid.h"
#include <string.h>

static bool ready(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && doubtech_hid_request_ready_slot(slot);
}

static bool release(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && doubtech_hid_request_release_slot(slot);
}

static bool arm(void *context, uint8_t slot)
{
    (void)context;
    return slot >= 1 && slot <= 3 && doubtech_hid_request_arm_slot(slot);
}

static bool send_input(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    (void)context;
    if (slot < 1 || slot > 3 || !input) return false;
    switch (input->kind) {
    case KVM_ROUTER_KEYBOARD: return doubtech_hid_request_keyboard_slot(slot, input->keyboard);
    case KVM_ROUTER_POINTER:
        return doubtech_hid_request_mouse_slot(slot, input->buttons, input->dx, input->dy,
                                            input->wheel, input->pan);
    case KVM_ROUTER_CONSUMER: return doubtech_hid_request_consumer_slot(slot, input->consumer);
    default: return false;
    }
}

static void disconnect_guest(void *context, uint8_t slot)
{
    (void)context;
    if (slot >= 1 && slot <= 3) (void)doubtech_hid_request_disconnect_slot(slot);
}

kvm_router_output_t kvm_router_hid_output(void)
{
    kvm_router_output_t output = {ready, release, arm, send_input, disconnect_guest};
    return output;
}

bool kvm_router_hid_central_msg(const doubtech_hid_central_input_t *in, kvm_hub_msg_t *out)
{
    if (!in || !out) return false;
    if (in->kind != DOUBTECH_HID_CENTRAL_KEYBOARD && in->kind != DOUBTECH_HID_CENTRAL_MOUSE)
        return false;
    memset(out, 0, sizeof(*out));
    if (in->lost) {
        out->type = KVM_HUB_MSG_LOST;
        out->input.kind = in->kind == DOUBTECH_HID_CENTRAL_KEYBOARD ? KVM_ROUTER_KEYBOARD
                                                                     : KVM_ROUTER_POINTER;
        return true;
    }
    out->type = KVM_HUB_MSG_INPUT;
    if (in->kind == DOUBTECH_HID_CENTRAL_KEYBOARD) {
        out->input.kind = KVM_ROUTER_KEYBOARD;
        memcpy(out->input.keyboard, in->keys, sizeof(out->input.keyboard));
    } else {
        out->input.kind = KVM_ROUTER_POINTER;
        out->input.buttons = in->buttons;
        out->input.dx = in->dx;
        out->input.dy = in->dy;
        out->input.wheel = in->wheel;
    }
    return true;
}
