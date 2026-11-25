/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure input hub. The target arm is taken on the first forwarded
 * report and dropped on every target change, yield and idle device loss, so the
 * hub never holds an output the host route may need. */
#include "input_hub.h"
#include <string.h>

static bool held(const kvm_input_hub_t *hub)
{
    static const uint8_t none[8] = {0};
    return hub->buttons != 0 || memcmp(hub->keys, none, sizeof none) != 0;
}

/* Sends an all-up keyboard and zero pointer report to the armed slot. */
static void send_all_up(kvm_input_hub_t *hub)
{
    kvm_router_input_t up;
    if (hub->armed_slot == 0 || hub->output.send == NULL) {
        return;
    }
    memset(&up, 0, sizeof up);
    up.kind = KVM_ROUTER_KEYBOARD;
    (void)hub->output.send(hub->context, hub->armed_slot, &up);
    memset(&up, 0, sizeof up);
    up.kind = KVM_ROUTER_POINTER;
    (void)hub->output.send(hub->context, hub->armed_slot, &up);
    memset(hub->keys, 0, sizeof hub->keys);
    hub->buttons = 0;
}

static void drop_arm(kvm_input_hub_t *hub)
{
    if (hub->armed_slot == 0) {
        return;
    }
    if (hub->output.release != NULL) {
        (void)hub->output.release(hub->context, hub->armed_slot);
    }
    hub->armed_slot = 0;
}

void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context)
{
    memset(hub, 0, sizeof *hub);
    hub->output = output;
    hub->context = context;
}

bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target)
{
    if (target > KVM_INPUT_HUB_MAX_TARGET) {
        return false;
    }
    if (target == hub->target) {
        return true;
    }
    send_all_up(hub);
    drop_arm(hub);
    memset(hub->keys, 0, sizeof hub->keys);
    hub->buttons = 0;
    hub->target = target;
    return true;
}

kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed)
{
    if (router_armed) {
        kvm_input_hub_yield(hub);
        return KVM_HUB_DROPPED_HOST_ARMED;
    }
    if (hub->target == 0) {
        return KVM_HUB_FORWARDED;
    }
    if (hub->output.ready == NULL || !hub->output.ready(hub->context, hub->target)) {
        return KVM_HUB_DROPPED_NOT_READY;
    }
    if (hub->armed_slot != hub->target) {
        if (hub->output.arm == NULL || !hub->output.arm(hub->context, hub->target)) {
            return KVM_HUB_DROPPED_NOT_READY;
        }
        hub->armed_slot = hub->target;
    }
    if (hub->output.send == NULL || !hub->output.send(hub->context, hub->target, input)) {
        return KVM_HUB_DROPPED_NOT_READY;
    }
    if (input->kind == KVM_ROUTER_KEYBOARD) {
        memcpy(hub->keys, input->keyboard, sizeof hub->keys);
    } else if (input->kind == KVM_ROUTER_POINTER) {
        hub->buttons = input->buttons;
    }
    return KVM_HUB_FORWARDED;
}

void kvm_input_hub_yield(kvm_input_hub_t *hub)
{
    send_all_up(hub);
    drop_arm(hub);
}

void kvm_input_hub_device_lost(kvm_input_hub_t *hub)
{
    bool was_held = held(hub);
    send_all_up(hub);
    if (!was_held) {
        drop_arm(hub);
    }
}
