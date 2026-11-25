/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure input hub. The target arm is taken on the first forwarded
 * report and dropped on every target change, yield and idle device loss, so the
 * hub never holds an output the host route may need. The hub never emits while
 * the router is armed, and held state survives a failed all-up for a retry. */
#include "input_hub.h"
#include <string.h>

static bool keys_held(const kvm_input_hub_t *hub)
{
    static const uint8_t none[8] = {0};
    return memcmp(hub->keys, none, sizeof none) != 0;
}

static bool held(const kvm_input_hub_t *hub)
{
    return keys_held(hub) || hub->buttons != 0 || hub->consumer != 0;
}

/* Slot that owns held state: the armed slot, else the target (for retries). */
static uint8_t owner_slot(const kvm_input_hub_t *hub)
{
    return hub->armed_slot != 0 ? hub->armed_slot : hub->target;
}

/* Sends all-up keyboard and zero pointer to an armed slot, plus zero consumer when held. Each part
 * clears its held state only when its send succeeds. Returns true when every send
 * succeeded. */
static bool send_all_up(kvm_input_hub_t *hub)
{
    kvm_router_input_t up;
    uint8_t slot = owner_slot(hub);
    bool ok = true;
    if (slot == 0 || (hub->armed_slot == 0 && !held(hub))) {
        return true;
    }
    if (hub->output.send == NULL) {
        return false;
    }
    memset(&up, 0, sizeof up);
    up.kind = KVM_ROUTER_KEYBOARD;
    if (hub->output.send(hub->context, slot, &up)) {
        memset(hub->keys, 0, sizeof hub->keys);
    } else {
        ok = false;
    }
    memset(&up, 0, sizeof up);
    up.kind = KVM_ROUTER_POINTER;
    if (hub->output.send(hub->context, slot, &up)) {
        hub->buttons = 0;
    } else {
        ok = false;
    }
    if (hub->consumer != 0) {
        memset(&up, 0, sizeof up);
        up.kind = KVM_ROUTER_CONSUMER;
        if (hub->output.send(hub->context, slot, &up)) {
            hub->consumer = 0;
        } else {
            ok = false;
        }
    }
    return ok;
}

/* Releases the arm; keeps armed_slot when the release is uncertain. */
static bool drop_arm(kvm_input_hub_t *hub)
{
    if (hub->armed_slot == 0) {
        return true;
    }
    if (hub->output.release != NULL && !hub->output.release(hub->context, hub->armed_slot)) {
        return false;
    }
    hub->armed_slot = 0;
    return true;
}

static void clear_local(kvm_input_hub_t *hub)
{
    memset(hub->keys, 0, sizeof hub->keys);
    hub->buttons = 0;
    hub->consumer = 0;
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
    if (!send_all_up(hub) || !drop_arm(hub)) {
        return false;
    }
    hub->target = target;
    return true;
}

kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed)
{
    if (router_armed) {
        /* The host route owns the output: emit nothing, forget local state. */
        clear_local(hub);
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
    } else if (input->kind == KVM_ROUTER_CONSUMER) {
        hub->consumer = input->consumer;
    }
    return KVM_HUB_FORWARDED;
}

void kvm_input_hub_yield(kvm_input_hub_t *hub)
{
    (void)send_all_up(hub);
    (void)drop_arm(hub);
}

void kvm_input_hub_device_lost(kvm_input_hub_t *hub)
{
    bool was_held = held(hub);
    if (send_all_up(hub) && !was_held) {
        (void)drop_arm(hub);
    }
}
