/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure input hub. The target arm is taken on the first forwarded
 * report and dropped on every target change, yield and idle device loss, so the
 * hub never holds an output the host route may need. The hub never emits while
 * the router is armed, never arms to retry, and keeps held state across a failed
 * all-up on a ready slot only; a not-ready slot is dead and its state is abandoned.
 * Keyboard chords (Left Ctrl + Left Alt + key) switch the target. */
#include "input_hub.h"
#include <string.h>

#define MOD_LCTRL_LALT 0x05u
#define HID_KEY_1 0x1Eu
#define HID_KEY_3 0x20u
#define HID_KEY_0 0x27u
#define HID_KEY_ESC 0x29u

static bool keys_held(const kvm_input_hub_t *hub)
{
    static const uint8_t none[8] = {0};
    return memcmp(hub->keys, none, sizeof none) != 0;
}

static bool held(const kvm_input_hub_t *hub)
{
    return keys_held(hub) || hub->buttons != 0 || hub->consumer != 0;
}

static void clear_held(kvm_input_hub_t *hub)
{
    memset(hub->keys, 0, sizeof hub->keys);
    hub->buttons = 0;
    hub->consumer = 0;
}

static void clear_local(kvm_input_hub_t *hub)
{
    clear_held(hub);
    hub->armed_slot = 0;
    memset(hub->raw_keys, 0, sizeof hub->raw_keys);
    hub->chord_latched = false;
}

/* True when the output reports the slot not ready: it cannot hold keys down. */
static bool slot_dead(const kvm_input_hub_t *hub, uint8_t slot)
{
    return hub->output.ready == NULL || !hub->output.ready(hub->context, slot);
}

/* Sends all-up keyboard and zero pointer to the armed slot, plus zero consumer when held.
 * Each part clears its held state only when its send succeeds. Never sends to an unarmed
 * slot. A dead slot has its held state abandoned and counts as success. Returns true when
 * nothing is left held. */
static bool send_all_up(kvm_input_hub_t *hub)
{
    kvm_router_input_t up;
    uint8_t slot = hub->armed_slot;
    bool ok = true;
    if (slot == 0 || slot_dead(hub, slot)) {
        clear_held(hub);
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
    if (!ok && slot_dead(hub, slot)) {
        clear_held(hub);
        return true;
    }
    return ok;
}

/* Releases the arm; keeps armed_slot when a ready slot's release is uncertain. */
static bool drop_arm(kvm_input_hub_t *hub)
{
    if (hub->armed_slot == 0) {
        return true;
    }
    if (hub->output.release != NULL && !hub->output.release(hub->context, hub->armed_slot) &&
        !slot_dead(hub, hub->armed_slot)) {
        return false;
    }
    hub->armed_slot = 0;
    return true;
}

void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context)
{
    memset(hub, 0, sizeof *hub);
    hub->output = output;
    hub->context = context;
}

bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed)
{
    if (target > KVM_INPUT_HUB_MAX_TARGET) {
        return false;
    }
    if (router_armed) {
        clear_local(hub);
        hub->target = target;
        return true;
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

/* Decides whether a keyboard report is a chord press. Exactly Left Ctrl + Left Alt and exactly
 * one non-modifier key; sets the target and whether it also releases everything. */
static bool hub_chord(const uint8_t keys[8], uint8_t *target, bool *release_all)
{
    uint8_t code = 0;
    int i;
    if (keys[0] != MOD_LCTRL_LALT || keys[1] != 0) {
        return false;
    }
    for (i = 2; i < 8; i++) {
        if (keys[i] != 0) {
            if (code != 0) {
                return false;
            }
            code = keys[i];
        }
    }
    *release_all = false;
    if (code >= HID_KEY_1 && code <= HID_KEY_3) {
        *target = (uint8_t)(code - HID_KEY_1 + 1u);
    } else if (code == HID_KEY_0) {
        *target = 0;
    } else if (code == HID_KEY_ESC) {
        *target = 0;
        *release_all = true;
    } else {
        return false;
    }
    return true;
}

static bool no_keys(const uint8_t keys[8])
{
    int i;
    for (i = 2; i < 8; i++) {
        if (keys[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Handles a keyboard report for chords. Returns true and sets *result when consumed. */
static bool chord_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                        kvm_hub_result_t *result)
{
    uint8_t target = 0;
    bool release_all = false;
    bool was_up = no_keys(hub->raw_keys);
    memcpy(hub->raw_keys, input->keyboard, sizeof hub->raw_keys);
    if (hub->chord_latched) {
        if (no_keys(input->keyboard)) {
            hub->chord_latched = false;
        }
        *result = KVM_HUB_SWALLOWED;
        return true;
    }
    /* A chord key must be newly pressed with nothing else held. */
    if (!was_up || !hub_chord(input->keyboard, &target, &release_all)) {
        return false;
    }
    hub->chord_latched = true;
    *result = KVM_HUB_SWALLOWED;
    if (target != hub->target) {
        if (kvm_input_hub_set_target(hub, target, false)) {
            *result = KVM_HUB_TARGET_CHANGED;
        }
    } else if (release_all) {
        (void)kvm_input_hub_yield(hub);
    }
    return true;
}

kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed)
{
    kvm_hub_result_t chord_result;
    if (router_armed) {
        /* The host route owns the output: emit nothing, forget local state. */
        clear_local(hub);
        return KVM_HUB_DROPPED_HOST_ARMED;
    }
    if (input->kind == KVM_ROUTER_KEYBOARD && chord_input(hub, input, &chord_result)) {
        return chord_result;
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

bool kvm_input_hub_yield(kvm_input_hub_t *hub)
{
    /* A failed all-up keeps the arm too, so a retry can still reach the held slot. */
    return send_all_up(hub) && drop_arm(hub);
}

void kvm_input_hub_device_lost(kvm_input_hub_t *hub, bool router_armed)
{
    bool was_held;
    if (router_armed) {
        clear_local(hub);
        return;
    }
    was_held = held(hub);
    if (send_all_up(hub) && !was_held) {
        (void)drop_arm(hub);
    }
}
