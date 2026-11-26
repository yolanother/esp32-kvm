/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the portable input hub with a recording output sink. Cases cover
 * lazy arming, host-armed arbitration, unready targets, target changes, yield,
 * device loss and the local sink without a physical board. The PC cases bind a
 * second recording output for This PC (target 0) that logs into the same call
 * list, so the order across PC and guest outputs is checked. */
#include "input_hub.h"
#include <assert.h>
#include <string.h>

#define MAX_CALLS 32

typedef enum { CALL_ARM, CALL_RELEASE, CALL_SEND } call_kind_t;

typedef struct {
    call_kind_t kind;
    uint8_t slot;
    kvm_router_input_t input;
    bool pc; /* made on the PC output (target 0), not the guest output */
} call_t;

typedef struct {
    bool ready;
    bool send_ok;
    bool release_ok;
    call_t calls[MAX_CALLS];
    int count;
} fake_t;

static bool ready(void *context, uint8_t slot)
{
    fake_t *f = context;
    (void)slot;
    return f->ready;
}
static void record(fake_t *f, call_kind_t kind, uint8_t slot, const kvm_router_input_t *in)
{
    assert(f->count < MAX_CALLS);
    memset(&f->calls[f->count], 0, sizeof f->calls[f->count]);
    f->calls[f->count].kind = kind;
    f->calls[f->count].slot = slot;
    if (in != NULL) {
        f->calls[f->count].input = *in;
    }
    f->count++;
}
static bool release(void *context, uint8_t slot)
{
    fake_t *f = context;
    record(f, CALL_RELEASE, slot, NULL);
    return f->release_ok;
}
static bool arm(void *context, uint8_t slot)
{
    record(context, CALL_ARM, slot, NULL);
    return true;
}
static bool send(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    fake_t *f = context;
    record(f, CALL_SEND, slot, input);
    return f->send_ok;
}

static kvm_router_output_t output(void)
{
    kvm_router_output_t o = {ready, release, arm, send, NULL};
    return o;
}

static void setup(kvm_input_hub_t *hub, fake_t *f)
{
    memset(f, 0, sizeof *f);
    f->ready = true;
    f->send_ok = true;
    f->release_ok = true;
    kvm_input_hub_init(hub, output(), f);
}

static kvm_router_input_t key(uint8_t code)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof in);
    in.kind = KVM_ROUTER_KEYBOARD;
    in.keyboard[2] = code;
    return in;
}

static bool is_all_up(const call_t *c, kvm_router_input_kind_t kind)
{
    static const uint8_t none[8] = {0};
    return c->kind == CALL_SEND && c->input.kind == kind &&
           memcmp(c->input.keyboard, none, sizeof none) == 0 && c->input.buttons == 0 &&
           c->input.dx == 0 && c->input.dy == 0 && c->input.wheel == 0 && c->input.pan == 0;
}

static void forwards_to_ready_guest_after_arm(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    assert(kvm_input_hub_set_target(&hub, 2, false));
    assert(f.count == 0);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 2);
    assert(f.calls[0].kind == CALL_ARM && f.calls[0].slot == 2);
    assert(f.calls[1].kind == CALL_SEND && f.calls[1].slot == 2 && f.calls[1].input.keyboard[2] == 4);
    assert(hub.armed_slot == 2 && hub.keys[2] == 4);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 3 && f.calls[2].kind == CALL_SEND);
}

static void drops_when_router_armed(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &in, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(f.count == 0);
    assert(hub.armed_slot == 0 && hub.target == 1 && hub.keys[2] == 0);
    assert(kvm_input_hub_input(&hub, &in, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(f.count == 0);
}

static void drops_when_target_not_ready(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    f.ready = false;
    kvm_input_hub_set_target(&hub, 3, false);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(f.count == 0 && hub.target == 3 && hub.armed_slot == 0);
}

static void set_target_sends_all_up_then_arms_new(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_set_target(&hub, 2, false));
    assert(f.count == 3);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].slot == 1);
    assert(f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 1);
    assert(hub.target == 2 && hub.armed_slot == 0);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.calls[0].kind == CALL_ARM && f.calls[0].slot == 2);
    assert(f.calls[1].kind == CALL_SEND && f.calls[1].slot == 2);
}

static void yield_releases_and_clears_arm(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    kvm_input_hub_yield(&hub, false);
    assert(f.count == 3 && f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 1);
    assert(hub.armed_slot == 0 && hub.keys[2] == 0);
    f.count = 0;
    kvm_input_hub_yield(&hub, false);
    assert(f.count == 0);
}

static void device_lost_sends_all_up_keyboard_and_zero_mouse(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag;
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    memset(&drag, 0, sizeof drag);
    drag.kind = KVM_ROUTER_POINTER;
    drag.buttons = 1;
    kvm_input_hub_input(&hub, &drag, false);
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 2);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].slot == 1);
    assert(hub.target == 1 && hub.buttons == 0 && hub.keys[2] == 0);
    /* Nothing is held now, so a second loss releases the arm. */
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 3 && f.calls[2].kind == CALL_RELEASE && hub.armed_slot == 0);
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 0);
}

static void target_zero_is_noop_sink(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    assert(hub.target == 0);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    kvm_input_hub_device_lost(&hub, false);
    kvm_input_hub_yield(&hub, false);
    assert(kvm_input_hub_set_target(&hub, 0, false));
    assert(f.count == 0);
}

static void invalid_target_rejected(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    setup(&hub, &f);
    assert(kvm_input_hub_set_target(&hub, 1, false));
    assert(!kvm_input_hub_set_target(&hub, 4, false));
    assert(!kvm_input_hub_set_target(&hub, 255, false));
    assert(hub.target == 1 && f.count == 0);
}

static void yield_before_arm_sends_all_up_then_releases_previous_target(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 3, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    kvm_input_hub_yield(&hub, false); /* the caller does this before the router arms */
    assert(f.count == 3);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 3);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].slot == 3);
    assert(f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 3);
    assert(hub.armed_slot == 0);
}

static void failed_all_up_keeps_held_state_and_device_lost_retries(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(hub.keys[2] == 4 && hub.armed_slot == 1);
    assert(!kvm_input_hub_set_target(&hub, 2, false) && hub.target == 1 && hub.keys[2] == 4);
    f.send_ok = true;
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(hub.keys[2] == 0);
    assert(kvm_input_hub_set_target(&hub, 2, false) && hub.target == 2);
}

static void failed_release_keeps_arm(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.release_ok = false;
    assert(!kvm_input_hub_set_target(&hub, 2, false));
    assert(hub.armed_slot == 1 && hub.target == 1);
    f.release_ok = true;
    assert(kvm_input_hub_set_target(&hub, 2, false) && hub.armed_slot == 0);
}

static void key_held_while_target_goes_not_ready_is_still_released(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 2, false);
    kvm_input_hub_input(&hub, &in, false);
    f.ready = false;
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_DROPPED_NOT_READY);
    f.send_ok = false;
    f.count = 0;
    /* Known disconnected (slot sample or DISCONNECTED): the slot cannot hold keys,
     * so its state is abandoned without emitting. */
    kvm_input_hub_set_live(&hub, (uint8_t)(KVM_INPUT_HUB_ALL_LIVE & ~(1u << 1)));
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 0 && hub.keys[2] == 0 && hub.armed_slot == 0);
}

static void consumer_held_is_zeroed_on_target_change_and_device_lost(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t vol;
    setup(&hub, &f);
    memset(&vol, 0, sizeof vol);
    vol.kind = KVM_ROUTER_CONSUMER;
    vol.consumer = 0xE9;
    kvm_input_hub_set_target(&hub, 1, false);
    assert(kvm_input_hub_input(&hub, &vol, false) == KVM_HUB_FORWARDED);
    assert(hub.consumer == 0xE9);
    f.count = 0;
    assert(kvm_input_hub_set_target(&hub, 2, false));
    assert(f.count == 4 && is_all_up(&f.calls[2], KVM_ROUTER_CONSUMER) && f.calls[2].slot == 1);
    assert(f.calls[2].input.consumer == 0 && hub.consumer == 0);
    kvm_input_hub_input(&hub, &vol, false);
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 3 && is_all_up(&f.calls[2], KVM_ROUTER_CONSUMER) && f.calls[2].slot == 2);
    assert(hub.consumer == 0);
}

static void guest_targets_use_matching_router_slot_and_zero_emits_nothing(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    uint8_t t;
    for (t = 1; t <= 3; t++) {
        setup(&hub, &f);
        kvm_input_hub_set_target(&hub, t, false);
        kvm_input_hub_input(&hub, &in, false);
        assert(f.calls[0].kind == CALL_ARM && f.calls[0].slot == t);
        assert(f.calls[1].kind == CALL_SEND && f.calls[1].slot == t);
    }
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 0, false);
    kvm_input_hub_input(&hub, &in, false);
    kvm_input_hub_input(&hub, &in, true);
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 0);
}

static kvm_router_input_t kbd(uint8_t mod, uint8_t k0, uint8_t k1)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof in);
    in.kind = KVM_ROUTER_KEYBOARD;
    in.keyboard[0] = mod;
    in.keyboard[2] = k0;
    in.keyboard[3] = k1;
    return in;
}

#define LCA 0x05
#define KEY_1 0x1E
#define KEY_2 0x1F
#define KEY_0 0x27
#define KEY_ESC 0x29

static void ctrl_alt_1_selects_guest_1(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    setup(&hub, &f);
    assert(kvm_input_hub_input(&hub, &mods, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    assert(hub.target == 1 && f.count == 0);
    chord = kbd(LCA, KEY_2, 0);
    kvm_input_hub_input(&hub, &mods, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED && hub.target == 2);
}

static void ctrl_alt_0_selects_pc(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t chord = kbd(LCA, KEY_0, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 2, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    assert(hub.target == 0);
}

static void ctrl_alt_esc_releases_all_to_pc(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = kbd(LCA, 0, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_ESC, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 3, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    assert(hub.target == 0 && hub.armed_slot == 0 && hub.keys[0] == 0);
    assert(f.count == 3 && is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 3);
    assert(f.calls[2].kind == CALL_RELEASE);
    /* Already on This PC: swallowed, unchanged. */
    kvm_input_hub_input(&hub, &up, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWALLOWED);
}

static void chord_with_extra_modifier_is_forwarded(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t shifted = kbd(LCA | 0x02, KEY_1, 0);
    kvm_router_input_t gui = kbd(LCA | 0x08, KEY_1, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    assert(kvm_input_hub_input(&hub, &shifted, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &gui, false) == KVM_HUB_FORWARDED);
    assert(hub.target == 1);
}

static void right_ctrl_alt_is_not_a_chord(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t rctrl = kbd(0x10 | 0x04, KEY_1, 0);
    kvm_router_input_t lctrl_ralt = kbd(0x01 | 0x40, KEY_1, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    assert(kvm_input_hub_input(&hub, &rctrl, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &lctrl_ralt, false) == KVM_HUB_FORWARDED);
    assert(hub.target == 1);
}

static void chord_key_not_forwarded_after_target_change(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    kvm_router_input_t released_alt = kbd(0x01, KEY_1, 0);
    kvm_router_input_t plain = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 2, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED && hub.target == 1);
    f.count = 0;
    /* Still held, even with a modifier change: swallowed, nothing reaches the new target. */
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &released_alt, false) == KVM_HUB_SWALLOWED);
    assert(f.count == 0);
    /* All keys up ends the swallow; the next press is forwarded. */
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &plain, false) == KVM_HUB_FORWARDED);
    assert(f.count == 2 && f.calls[1].slot == 1 && f.calls[1].input.keyboard[2] == 4);
}

static void chord_does_not_fire_when_another_key_is_held(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t a_held = kbd(LCA, 4, 0);
    kvm_router_input_t both = kbd(LCA, 4, KEY_1);
    kvm_router_input_t one_first = kbd(0, KEY_1, 0);
    kvm_router_input_t mods_added = kbd(LCA, KEY_1, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 3, false);
    kvm_input_hub_input(&hub, &a_held, false);
    assert(kvm_input_hub_input(&hub, &both, false) == KVM_HUB_FORWARDED && hub.target == 3);
    kvm_input_hub_input(&hub, &up, false);
    /* A key held before the modifiers is not a chord press. */
    kvm_input_hub_input(&hub, &one_first, false);
    assert(kvm_input_hub_input(&hub, &mods_added, false) == KVM_HUB_FORWARDED && hub.target == 3);
}

static void chord_with_failed_old_target_stays_swallowed_and_retries(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = kbd(LCA, 0, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false; /* transient failure on a ready target */
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWITCH_FAILED);
    assert(hub.target == 1 && hub.keys[0] == LCA);
    f.send_ok = true;
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_TARGET_CHANGED);
    assert(hub.target == 2);
}

static void all_up_old_target_before_switch(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].slot == 1);
    assert(f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 1 && f.count == 3);
}

static void chord_ignored_while_router_armed(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    setup(&hub, &f);
    assert(kvm_input_hub_input(&hub, &chord, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(hub.target == 0 && f.count == 0);
}

static void set_target_abandons_dead_old_target(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.ready = false;
    f.send_ok = false;
    f.release_ok = false;
    /* Not ready alone keeps the switch waiting (the slot may be busy, still armed). */
    assert(!kvm_input_hub_set_target(&hub, 2, false) && hub.target == 1 && hub.armed_slot == 1);
    /* Known disconnected: the dead link cannot hold keys, the switch goes through. */
    hub.live_mask = (uint8_t)(KVM_INPUT_HUB_ALL_LIVE & ~1u); /* gone, arm not yet abandoned */
    assert(kvm_input_hub_set_target(&hub, 2, false));
    assert(hub.target == 2 && hub.armed_slot == 0 && hub.keys[2] == 0);
}

static void chord_abandons_dead_old_target(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_ESC, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.ready = false;
    f.send_ok = false;
    f.release_ok = false;
    hub.live_mask = (uint8_t)(KVM_INPUT_HUB_ALL_LIVE & ~1u); /* slot 1 known disconnected */
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    assert(hub.target == 0 && hub.armed_slot == 0 && hub.keys[0] == 0);
}

static void yield_returns_whether_everything_was_released(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    assert(kvm_input_hub_yield(&hub, false));
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    assert(!kvm_input_hub_yield(&hub, false) && hub.keys[2] == 4 && hub.armed_slot == 1);
    f.send_ok = true;
    assert(kvm_input_hub_yield(&hub, false) && hub.armed_slot == 0);
    kvm_input_hub_input(&hub, &in, false);
    f.ready = false;
    f.send_ok = false;
    assert(!kvm_input_hub_yield(&hub, false) && hub.armed_slot == 1 && hub.keys[2] == 4);
    hub.live_mask = (uint8_t)(KVM_INPUT_HUB_ALL_LIVE & ~1u); /* slot 1 known disconnected */
    assert(kvm_input_hub_yield(&hub, false) && hub.armed_slot == 0 && hub.keys[2] == 0);
}

/* ready() false while the slot is still connected: the request bridge is busy
 * with an earlier timed-out request. The channel may still be armed and holding
 * keys, so the arm and held state stay, the hub is dirty, and flush resends the
 * wanted state once the slot answers again. */
static void busy_slot_keeps_arm_and_flush_resends(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t up = key(0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.ready = false;
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(hub.dirty && hub.armed_slot == 1 && hub.keys[2] == 4);
    kvm_input_hub_device_lost(&hub, false);
    assert(!kvm_input_hub_yield(&hub, false));
    assert(!kvm_input_hub_set_target(&hub, 2, false) && hub.target == 1);
    assert(f.count == 0 && hub.armed_slot == 1 && hub.keys[2] == 4);
    assert(!kvm_input_hub_flush(&hub, false) && hub.dirty);
    f.ready = true;
    assert(kvm_input_hub_flush(&hub, false) && !hub.dirty);
    assert(f.count == 1 && is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(hub.keys[2] == 0 && hub.armed_slot == 1);
    assert(kvm_input_hub_flush(&hub, false) && f.count == 1); /* clean: nothing more */
}

/* A dropped key-up alone (no loss) is resent with the device's current state;
 * a dropped press is delivered late rather than lost. */
static void dropped_reports_are_resent_as_current_state(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t other = key(5);
    kvm_router_input_t drag;
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    assert(kvm_input_hub_input(&hub, &other, false) == KVM_HUB_DROPPED_NOT_READY);
    memset(&drag, 0, sizeof drag);
    drag.kind = KVM_ROUTER_POINTER;
    drag.buttons = 2;
    assert(kvm_input_hub_input(&hub, &drag, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(!kvm_input_hub_flush(&hub, false) && hub.dirty); /* the slot still refuses */
    f.send_ok = true;
    f.count = 0;
    assert(kvm_input_hub_flush(&hub, false));
    assert(f.count == 2 && f.calls[0].input.kind == KVM_ROUTER_KEYBOARD &&
           f.calls[0].input.keyboard[2] == 5);
    assert(f.calls[1].input.kind == KVM_ROUTER_POINTER && f.calls[1].input.buttons == 2 &&
           f.calls[1].input.dx == 0);
    assert(hub.keys[2] == 5 && hub.buttons == 2);
    /* Router armed: a dirty hub abandons without emitting. */
    f.send_ok = false;
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_flush(&hub, true) && f.count == 0 && hub.armed_slot == 0 && !hub.dirty);
}

/* Only a slot known disconnected is abandoned without a confirmed release; a
 * successful arm marks the slot live again. */
static void known_disconnected_slot_is_abandoned(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    assert(hub.live_mask == KVM_INPUT_HUB_ALL_LIVE);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    kvm_input_hub_set_live(&hub, 0x05); /* slot 2 gone: slot 1 untouched */
    kvm_input_hub_set_live(&hub, 0x07);
    assert(hub.armed_slot == 1 && f.count == 0);
    kvm_input_hub_set_live(&hub, 0x06);
    assert(f.count == 0 && hub.armed_slot == 0 && hub.keys[2] == 0 && !hub.dirty);
    assert(kvm_input_hub_flush(&hub, false));
    /* The guest comes back: the hub re-arms it and knows it is live again. */
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(hub.armed_slot == 1 && (hub.live_mask & 1u));
    /* A dirty hub on a slot found gone abandons on flush. */
    f.ready = false;
    kvm_input_hub_input(&hub, &in, false);
    assert(hub.dirty);
    hub.live_mask = 0x06;
    f.count = 0;
    assert(kvm_input_hub_flush(&hub, false) && f.count == 0 && hub.armed_slot == 0 && !hub.dirty);
}

static void retries_never_emit_while_router_armed(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    kvm_input_hub_device_lost(&hub, false);
    assert(hub.keys[2] == 4);
    f.send_ok = true;
    f.count = 0;
    kvm_input_hub_device_lost(&hub, true);
    assert(f.count == 0 && hub.keys[2] == 0 && hub.armed_slot == 0);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    kvm_input_hub_device_lost(&hub, false);
    f.send_ok = true;
    f.count = 0;
    assert(kvm_input_hub_set_target(&hub, 2, true) && hub.target == 2);
    assert(f.count == 0 && hub.keys[2] == 0 && hub.armed_slot == 0);
    /* No state and no arm: nothing is emitted on an unarmed slot. */
    kvm_input_hub_device_lost(&hub, false);
    assert(kvm_input_hub_yield(&hub, false) && f.count == 0);
}

static void chord_to_selected_guest_releases_held_modifiers(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    kvm_router_input_t alt_up = kbd(0x01, KEY_1, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    int i;
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &mods, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWALLOWED);
    assert(hub.keys[0] == 0 && hub.target == 1);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    kvm_input_hub_input(&hub, &alt_up, false);
    kvm_input_hub_input(&hub, &up, false);
    for (i = 0; i < f.count; i++) {
        assert(!(f.calls[i].kind == CALL_SEND && f.calls[i].input.kind == KVM_ROUTER_KEYBOARD &&
                 f.calls[i].input.keyboard[2] == KEY_1));
    }
}

static void esc_on_pc_emits_nothing(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t chord = kbd(LCA, KEY_ESC, 0);
    setup(&hub, &f);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWALLOWED);
    assert(f.count == 0 && hub.target == 0);
}

static void yield_while_router_armed_abandons_without_emitting(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    assert(!kvm_input_hub_yield(&hub, false));
    f.send_ok = true;
    f.count = 0;
    assert(kvm_input_hub_yield(&hub, true) && f.count == 0);
    assert(hub.armed_slot == 0 && hub.keys[2] == 0);
}

static void device_lost_resets_chord_tracking(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t held_a = kbd(0, 4, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    setup(&hub, &f);
    kvm_input_hub_input(&hub, &held_a, false);
    kvm_input_hub_device_lost(&hub, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    kvm_input_hub_device_lost(&hub, false);
    assert(!hub.chord_latched);
}

static void switch_failure_is_reported_and_retried(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &mods, false);
    f.send_ok = false;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWITCH_FAILED && hub.target == 1);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWITCH_FAILED);
    f.send_ok = true;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED && hub.target == 2);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED);
    assert(!hub.chord_latched);
}

static void failing_ready_target_never_kills_the_keyboard(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t to2 = kbd(LCA, KEY_2, 0);
    kvm_router_input_t to3 = kbd(LCA, KEY_1 + 2, 0);
    kvm_router_input_t esc = kbd(LCA, KEY_ESC, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &mods, false);
    f.send_ok = false;
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_SWITCH_FAILED);
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_SWITCH_FAILED);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWITCH_FAILED);
    assert(!hub.chord_latched && !hub.switch_pending && hub.target == 1 && hub.armed_slot == 1);
    assert(kvm_input_hub_input(&hub, &to3, false) == KVM_HUB_SWITCH_FAILED);
    assert(hub.pending_target == 3);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWITCH_FAILED);
    assert(!hub.chord_latched);
    assert(kvm_input_hub_input(&hub, &esc, false) == KVM_HUB_SWITCH_FAILED && hub.pending_target == 0);
    kvm_input_hub_input(&hub, &up, false);
    f.send_ok = true;
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_TARGET_CHANGED && hub.target == 2);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED && !hub.chord_latched);
    assert(kvm_input_hub_input(&hub, &esc, false) == KVM_HUB_TARGET_CHANGED && hub.target == 0);
}

static void set_target_clears_pending_switch(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t to2 = kbd(LCA, KEY_2, 0);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1, false);
    kvm_input_hub_input(&hub, &mods, false);
    f.send_ok = false;
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_SWITCH_FAILED && hub.switch_pending);
    f.send_ok = true;
    assert(kvm_input_hub_set_target(&hub, 3, false) && !hub.switch_pending && hub.target == 3);
}

/* Hub on target 1 holding key 4, button 1 and a consumer usage. */
static void hold_everything(kvm_input_hub_t *hub, fake_t *f)
{
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag, vol;
    setup(hub, f);
    kvm_input_hub_set_target(hub, 1, false);
    kvm_input_hub_input(hub, &in, false);
    memset(&drag, 0, sizeof drag);
    drag.kind = KVM_ROUTER_POINTER;
    drag.buttons = 1;
    kvm_input_hub_input(hub, &drag, false);
    memset(&vol, 0, sizeof vol);
    vol.kind = KVM_ROUTER_CONSUMER;
    vol.consumer = 0xE9;
    kvm_input_hub_input(hub, &vol, false);
    f->count = 0;
}

static void keyboard_lost_releases_only_keyboard_state(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    hold_everything(&hub, &f);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_KEYBOARD, false);
    assert(f.count == 2);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_CONSUMER) && f.calls[1].input.consumer == 0);
    assert(hub.keys[2] == 0 && hub.consumer == 0);
    /* The mouse is still connected and still holds its button: untouched, armed. */
    assert(hub.buttons == 1 && hub.armed_slot == 1);
}

static void mouse_lost_releases_only_buttons(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    hold_everything(&hub, &f);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_POINTER, false);
    assert(f.count == 1 && is_all_up(&f.calls[0], KVM_ROUTER_POINTER) && f.calls[0].slot == 1);
    assert(hub.buttons == 0 && hub.keys[2] == 4 && hub.consumer == 0xE9 && hub.armed_slot == 1);
    /* Chord tracking belongs to the keyboard: a held key still blocks a chord. */
    assert(kvm_input_hub_input(&hub, &chord, false) != KVM_HUB_TARGET_CHANGED);
}

static void kind_loss_with_nothing_held_releases_the_arm(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    hold_everything(&hub, &f);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_KEYBOARD, false);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_POINTER, false);
    assert(hub.keys[2] == 0 && hub.buttons == 0 && hub.consumer == 0 && hub.armed_slot == 1);
    /* A later loss with nothing held drops the arm, as device_lost does. */
    f.count = 0;
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_POINTER, false);
    assert(hub.armed_slot == 0 && f.calls[f.count - 1].kind == CALL_RELEASE);
    /* Router armed: abandon without emitting. */
    hold_everything(&hub, &f);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_POINTER, true);
    assert(f.count == 0 && hub.armed_slot == 0 && hub.keys[2] == 0);
    /* Any other kind is a whole loss, exactly as kvm_input_hub_device_lost. */
    hold_everything(&hub, &f);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_CONSUMER, false);
    assert(f.count == 3 && hub.keys[2] == 0 && hub.buttons == 0 && hub.consumer == 0);
}

static void keyboard_lost_resets_chord_tracking(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t held_a = kbd(0, 4, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    setup(&hub, &f);
    kvm_input_hub_input(&hub, &held_a, false);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_KEYBOARD, false);
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_KEYBOARD, false);
    assert(!hub.chord_latched);
}

/* This PC is a stub until stage 3, so until the owner makes an explicit choice
 * since boot the hub keeps its target on a ready guest: the lowest-numbered one
 * when the current target is not a ready guest; a ready target is kept. */
static void auto_target_follows_ready_guests_until_explicit(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t to_pc = kbd(LCA, KEY_0, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    setup(&hub, &f);
    assert(hub.target == 0 && !hub.explicit_target);
    assert(!kvm_input_hub_auto_target(&hub, 0x00, false) && hub.target == 0); /* no guest */
    assert(kvm_input_hub_auto_target(&hub, 0x06, false) && hub.target == 2);  /* lowest ready */
    assert(!kvm_input_hub_auto_target(&hub, 0x07, false) && hub.target == 2); /* kept: still ready */
    assert(f.count == 0 && !hub.explicit_target);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED && hub.armed_slot == 2);
    /* Guest 2 drops: all-up and release on it first, then the lowest ready guest. */
    f.count = 0;
    assert(kvm_input_hub_auto_target(&hub, 0x05, false) && hub.target == 1);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 2);
    assert(hub.armed_slot == 0 && hub.keys[2] == 0);
    assert(!kvm_input_hub_auto_target(&hub, 0x00, false) && hub.target == 1); /* none: left alone */
    /* An explicit This PC (chord) is remembered: no automatic move any more. */
    kvm_input_hub_input(&hub, &up, false);
    assert(kvm_input_hub_input(&hub, &to_pc, false) == KVM_HUB_TARGET_CHANGED && hub.target == 0);
    assert(hub.explicit_target);
    assert(!kvm_input_hub_auto_target(&hub, 0x07, false) && hub.target == 0);
    /* Any set_target (touch, PLUS) is explicit too. */
    setup(&hub, &f);
    assert(kvm_input_hub_set_target(&hub, 3, false) && hub.explicit_target);
    assert(!kvm_input_hub_auto_target(&hub, 0x01, false) && hub.target == 3);
    /* An invalid target is not a choice. */
    setup(&hub, &f);
    assert(!kvm_input_hub_set_target(&hub, 9, false) && !hub.explicit_target);
    /* A switch that cannot release the old target is retried by the next call. */
    assert(kvm_input_hub_auto_target(&hub, 0x01, false) && hub.target == 1);
    kvm_input_hub_input(&hub, &in, false);
    f.send_ok = false;
    assert(!kvm_input_hub_auto_target(&hub, 0x02, false) && hub.target == 1);
    f.send_ok = true;
    assert(kvm_input_hub_auto_target(&hub, 0x02, false) && hub.target == 2 && !hub.explicit_target);
    /* With the router armed it moves without emitting. */
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_auto_target(&hub, 0x01, true) && hub.target == 1 && f.count == 0);
}

/* ---- This PC as a real output (a PC output is bound) ---- */

/* The PC output's own answers; every call is logged into the guest fake's list. */
typedef struct {
    fake_t *log;
    bool ready;
    bool send_ok;
    bool release_ok;
    bool arm_ok;
} pc_fake_t;

static void pc_record(pc_fake_t *p, call_kind_t kind, uint8_t slot, const kvm_router_input_t *in)
{
    assert(slot == 0); /* the PC callbacks always get slot 0 */
    record(p->log, kind, slot, in);
    p->log->calls[p->log->count - 1].pc = true;
}
static bool pc_ready(void *context, uint8_t slot)
{
    pc_fake_t *p = context;
    assert(slot == 0);
    return p->ready;
}
static bool pc_release(void *context, uint8_t slot)
{
    pc_fake_t *p = context;
    pc_record(p, CALL_RELEASE, slot, NULL);
    return p->release_ok;
}
static bool pc_arm(void *context, uint8_t slot)
{
    pc_fake_t *p = context;
    pc_record(p, CALL_ARM, slot, NULL);
    return p->arm_ok;
}
static bool pc_send(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    pc_fake_t *p = context;
    pc_record(p, CALL_SEND, slot, input);
    return p->send_ok;
}

static kvm_router_output_t pc_output(void)
{
    kvm_router_output_t o = {pc_ready, pc_release, pc_arm, pc_send, NULL};
    return o;
}

static void setup_pc(kvm_input_hub_t *hub, fake_t *f, pc_fake_t *p)
{
    setup(hub, f);
    memset(p, 0, sizeof *p);
    p->log = f;
    p->ready = true;
    p->send_ok = true;
    p->release_ok = true;
    p->arm_ok = true;
    kvm_input_hub_set_pc_output(hub, pc_output(), p);
}

static bool on_pc(const call_t *c, call_kind_t kind) { return c->pc && c->kind == kind && c->slot == 0; }
static bool on_guest(const call_t *c, call_kind_t kind, uint8_t slot)
{
    return !c->pc && c->kind == kind && c->slot == slot;
}

static kvm_router_input_t pointer(uint8_t buttons)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof in);
    in.kind = KVM_ROUTER_POINTER;
    in.buttons = buttons;
    return in;
}

static kvm_router_input_t consumer(uint16_t usage)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof in);
    in.kind = KVM_ROUTER_CONSUMER;
    in.consumer = usage;
    return in;
}

static void pc_target_forwards_when_enabled(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag = pointer(1);
    kvm_router_input_t vol = consumer(0xE9);
    setup_pc(&hub, &f, &p);
    assert(hub.pc_enabled && hub.target == 0 && f.count == 0);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 2 && on_pc(&f.calls[0], CALL_ARM) && on_pc(&f.calls[1], CALL_SEND));
    assert(f.calls[1].input.keyboard[2] == 4);
    assert(hub.pc_armed && hub.armed_slot == 0 && hub.keys[2] == 4);
    /* Already armed: only sends, pointer and consumer too. */
    assert(kvm_input_hub_input(&hub, &drag, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &vol, false) == KVM_HUB_FORWARDED);
    assert(f.count == 4 && on_pc(&f.calls[2], CALL_SEND) && f.calls[2].input.buttons == 1);
    assert(on_pc(&f.calls[3], CALL_SEND) && f.calls[3].input.consumer == 0xE9);
    assert(hub.buttons == 1 && hub.consumer == 0xE9);
    /* A refused arm keeps the PC unarmed and nothing is sent. */
    setup_pc(&hub, &f, &p);
    p.arm_ok = false;
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(f.count == 1 && on_pc(&f.calls[0], CALL_ARM) && !hub.pc_armed && hub.keys[2] == 0);
}

/* Not ready (suspended) is not known disconnected: held state and the arm stay,
 * the hub is dirty and flush resends the wanted state once the PC is ready. */
static void pc_not_ready_marks_dirty_and_flush_resends_want(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t other = key(5);
    kvm_router_input_t drag = pointer(2);
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    p.ready = false;
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &other, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(kvm_input_hub_input(&hub, &drag, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(hub.dirty && hub.pc_armed && hub.pc_live && hub.keys[2] == 4 && hub.buttons == 0);
    assert(!kvm_input_hub_flush(&hub, false) && hub.dirty && hub.keys[2] == 4);
    assert(f.count == 0);
    p.ready = true;
    assert(kvm_input_hub_flush(&hub, false) && !hub.dirty);
    assert(f.count == 2 && on_pc(&f.calls[0], CALL_SEND) && f.calls[0].input.keyboard[2] == 5);
    assert(on_pc(&f.calls[1], CALL_SEND) && f.calls[1].input.kind == KVM_ROUTER_POINTER &&
           f.calls[1].input.buttons == 2);
    assert(hub.keys[2] == 5 && hub.buttons == 2 && hub.pc_armed);
    assert(kvm_input_hub_flush(&hub, false) && f.count == 2); /* clean: nothing more */
    /* A device loss while the PC is not ready keeps what the PC holds; the all-up
     * follows on resume through flush. */
    p.ready = false;
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 0 && hub.keys[2] == 5 && hub.buttons == 2 && hub.dirty && hub.pc_armed);
    p.ready = true;
    assert(kvm_input_hub_flush(&hub, false) && f.count == 2);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].pc);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].pc);
    assert(hub.keys[2] == 0 && hub.buttons == 0);
    /* A PC that refuses while ready keeps the state dirty too. */
    kvm_input_hub_input(&hub, &in, false);
    p.send_ok = false;
    assert(kvm_input_hub_input(&hub, &other, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(hub.dirty && hub.keys[2] == 4);
    p.send_ok = true;
    assert(kvm_input_hub_flush(&hub, false) && hub.keys[2] == 5);
    /* Not ready and not armed: dropped, nothing to resend. */
    setup_pc(&hub, &f, &p);
    p.ready = false;
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(f.count == 0 && !hub.pc_armed && !hub.dirty);
}

/* Unmounted (bus reset, re-enumeration): the host dropped the PC's state, so the
 * hub forgets the PC's held state and arm without emitting, and a dead PC never
 * blocks a switch. */
static void pc_unlive_abandons_without_emitting(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag = pointer(1);
    kvm_router_input_t vol = consumer(0xE9);
    setup_pc(&hub, &f, &p);
    assert(hub.pc_live);
    kvm_input_hub_input(&hub, &in, false);
    kvm_input_hub_input(&hub, &drag, false);
    kvm_input_hub_input(&hub, &vol, false);
    f.count = 0;
    kvm_input_hub_set_pc_live(&hub, false);
    assert(f.count == 0 && !hub.pc_armed && !hub.pc_live && !hub.dirty);
    assert(hub.keys[2] == 0 && hub.buttons == 0 && hub.consumer == 0);
    assert(kvm_input_hub_flush(&hub, false) && kvm_input_hub_yield(&hub, false) && f.count == 0);
    /* Not ready and dirty, then unmounted: abandoned too; the switch goes through. */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    p.ready = false;
    p.send_ok = false;
    p.release_ok = false;
    kvm_input_hub_input(&hub, &drag, false);
    assert(hub.dirty && hub.pc_armed);
    f.count = 0;
    kvm_input_hub_set_pc_live(&hub, false);
    assert(kvm_input_hub_set_target(&hub, 1, false) && hub.target == 1);
    assert(f.count == 0 && !hub.pc_armed && hub.keys[2] == 0 && !hub.dirty);
    /* A guest's live mask never touches the PC, and the PC's never touches a guest. */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    kvm_input_hub_set_live(&hub, 0x00);
    assert(hub.pc_armed && hub.keys[2] == 4);
    kvm_input_hub_set_live(&hub, KVM_INPUT_HUB_ALL_LIVE);
    kvm_input_hub_set_target(&hub, 2, false);
    kvm_input_hub_input(&hub, &in, false);
    kvm_input_hub_set_pc_live(&hub, false);
    assert(hub.armed_slot == 2 && hub.keys[2] == 4);
    /* A successful PC arm proves the PC is live again (as a guest arm does). */
    kvm_input_hub_set_target(&hub, 0, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(hub.pc_armed && hub.pc_live && on_pc(&f.calls[0], CALL_ARM));
}

/* Chord to a guest: keyboard all-up, pointer zero, consumer zero to the PC, then
 * the PC release (only after every all-up), and only then the guest arm. */
static void chord_pc_to_guest_releases_pc_first(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_1, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag = pointer(1);
    kvm_router_input_t vol = consumer(0xE9);
    int i;
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &drag, false);
    kvm_input_hub_input(&hub, &vol, false);
    kvm_input_hub_input(&hub, &mods, false);
    assert(hub.pc_armed && hub.keys[0] == LCA);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED && hub.target == 1);
    assert(f.count == 4);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].pc);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].pc);
    assert(is_all_up(&f.calls[2], KVM_ROUTER_CONSUMER) && f.calls[2].pc &&
           f.calls[2].input.consumer == 0);
    assert(on_pc(&f.calls[3], CALL_RELEASE));
    assert(!hub.pc_armed && hub.armed_slot == 0 && hub.keys[0] == 0 && hub.buttons == 0 &&
           hub.consumer == 0);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 6 && on_guest(&f.calls[4], CALL_ARM, 1) && on_guest(&f.calls[5], CALL_SEND, 1));
    for (i = 4; i < f.count; i++) assert(!f.calls[i].pc);
    /* A PC that refuses the all-up while ready keeps the switch pending (guest rule). */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &mods, false);
    p.send_ok = false;
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_SWITCH_FAILED && hub.target == 0);
    for (i = 0; i < f.count; i++) assert(f.calls[i].kind != CALL_RELEASE);
    assert(hub.pc_armed && hub.keys[0] == LCA);
    p.send_ok = true;
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_TARGET_CHANGED && hub.target == 1);
    assert(!hub.pc_armed && on_pc(&f.calls[f.count - 1], CALL_RELEASE));
}

/* Chord to This PC: all-up on the old guest and its release first, then the PC arm. */
static void chord_guest_to_pc_releases_guest_first(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t to_pc = kbd(LCA, KEY_0, 0);
    kvm_router_input_t esc = kbd(LCA, KEY_ESC, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t in = key(4);
    kvm_router_input_t vol = consumer(0xE9);
    int i;
    setup_pc(&hub, &f, &p);
    assert(kvm_input_hub_set_target(&hub, 1, false));
    kvm_input_hub_input(&hub, &vol, false);
    kvm_input_hub_input(&hub, &mods, false);
    assert(hub.armed_slot == 1 && !hub.pc_armed);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &to_pc, false) == KVM_HUB_TARGET_CHANGED && hub.target == 0);
    assert(f.count == 4);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && !f.calls[0].pc && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && !f.calls[1].pc && f.calls[1].slot == 1);
    assert(is_all_up(&f.calls[2], KVM_ROUTER_CONSUMER) && !f.calls[2].pc && f.calls[2].slot == 1);
    assert(on_guest(&f.calls[3], CALL_RELEASE, 1));
    assert(hub.armed_slot == 0 && !hub.pc_armed);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 6 && on_pc(&f.calls[4], CALL_ARM) && on_pc(&f.calls[5], CALL_SEND));
    assert(hub.pc_armed && hub.armed_slot == 0);
    /* Esc releases everything to This PC the same way. */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_set_target(&hub, 2, false);
    kvm_input_hub_input(&hub, &mods, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &esc, false) == KVM_HUB_TARGET_CHANGED && hub.target == 0);
    assert(f.count == 3 && on_guest(&f.calls[2], CALL_RELEASE, 2));
    for (i = 0; i < f.count; i++) assert(!f.calls[i].pc);
    /* Esc while already on the PC releases modifiers forwarded before the chord. */
    kvm_input_hub_input(&hub, &up, false);
    kvm_input_hub_input(&hub, &mods, false);
    assert(hub.pc_armed && hub.keys[0] == LCA);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &esc, false) == KVM_HUB_SWALLOWED && hub.target == 0);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].pc);
    assert(on_pc(&f.calls[f.count - 1], CALL_RELEASE) && !hub.pc_armed && hub.keys[0] == 0);
}

static void yield_with_pc_armed_sends_all_up_to_pc(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t vol = consumer(0xE9);
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    kvm_input_hub_input(&hub, &vol, false);
    f.count = 0;
    assert(kvm_input_hub_yield(&hub, false));
    assert(f.count == 4);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].pc);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].pc);
    assert(is_all_up(&f.calls[2], KVM_ROUTER_CONSUMER) && f.calls[2].pc);
    assert(on_pc(&f.calls[3], CALL_RELEASE));
    assert(!hub.pc_armed && hub.keys[2] == 0 && hub.consumer == 0 && hub.target == 0);
    f.count = 0;
    assert(kvm_input_hub_yield(&hub, false) && f.count == 0);
    /* A ready PC that refuses: false, held state and arm kept for the retry. */
    kvm_input_hub_input(&hub, &in, false);
    p.send_ok = false;
    f.count = 0;
    assert(!kvm_input_hub_yield(&hub, false) && hub.pc_armed && hub.keys[2] == 4);
    assert(f.count == 2 && f.calls[0].pc && f.calls[1].pc); /* no release before the all-ups */
    /* A refused release keeps the arm too. */
    p.send_ok = true;
    p.release_ok = false;
    assert(!kvm_input_hub_yield(&hub, false) && hub.pc_armed && hub.keys[2] == 0);
    p.release_ok = true;
    assert(kvm_input_hub_yield(&hub, false) && !hub.pc_armed);
    /* With the router armed it abandons without emitting. */
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_yield(&hub, true) && f.count == 0 && !hub.pc_armed && hub.keys[2] == 0);
}

/* A suspended PC (live, not ready) cannot take the all-up now. Switching away or
 * yielding releases it through pc.release, which zeroes the state-based HID sink for
 * delivery on resume, so a sleeping PC never blocks the move to a guest. */
static void suspended_pc_is_released_through_release(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t mods = kbd(LCA, 0, 0);
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &mods, false);
    p.ready = false;
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &chord, false) == KVM_HUB_TARGET_CHANGED && hub.target == 2);
    assert(f.count == 1 && on_pc(&f.calls[0], CALL_RELEASE));
    assert(!hub.pc_armed && hub.keys[0] == 0 && !hub.dirty);
    /* Yield likewise. */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    p.ready = false;
    f.count = 0;
    assert(kvm_input_hub_yield(&hub, false));
    assert(f.count == 1 && on_pc(&f.calls[0], CALL_RELEASE) && !hub.pc_armed && hub.keys[2] == 0);
    /* A release the PC output does not confirm keeps everything (retry later). */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    p.ready = false;
    p.release_ok = false;
    assert(!kvm_input_hub_set_target(&hub, 1, false) && hub.target == 0);
    assert(hub.pc_armed && hub.keys[2] == 4 && hub.dirty);
    assert(!kvm_input_hub_yield(&hub, false) && hub.pc_armed);
    /* A device loss is not a switch: it keeps the guest rule (dirty, resent on resume). */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    p.ready = false;
    f.count = 0;
    kvm_input_hub_device_lost(&hub, false);
    assert(f.count == 0 && hub.pc_armed && hub.keys[2] == 4 && hub.dirty);
}

static void router_armed_drops_input_and_pc_gets_nothing(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t other = key(5);
    kvm_router_input_t drag = pointer(1);
    kvm_router_input_t vol = consumer(0xE9);
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    assert(hub.pc_armed);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &other, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &drag, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &vol, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(f.count == 0 && !hub.pc_armed && hub.keys[2] == 0 && hub.buttons == 0 && !hub.dirty);
    assert(hub.target == 0);
    /* Every other call with the router armed emits nothing either. */
    assert(kvm_input_hub_flush(&hub, true));
    kvm_input_hub_device_lost(&hub, true);
    kvm_input_hub_device_lost_kind(&hub, KVM_ROUTER_KEYBOARD, true);
    assert(kvm_input_hub_yield(&hub, true));
    assert(kvm_input_hub_set_target(&hub, 1, true) && hub.target == 1);
    assert(kvm_input_hub_set_target(&hub, 0, true) && hub.target == 0);
    assert(f.count == 0);
    /* The router disarms: input goes to the PC again, re-armed lazily. */
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 2 && on_pc(&f.calls[0], CALL_ARM) && on_pc(&f.calls[1], CALL_SEND));
}

/* A chord typed while the router is armed (the app drives a guest) is reported to
 * the caller, which forwards it to the app as a select request; the hub itself keeps
 * its target, clears local state, swallows the chord keys and emits nothing. */
static void chord_while_router_armed_returns_host_request(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    kvm_router_input_t to2 = kbd(LCA, KEY_2, 0);
    kvm_router_input_t to0 = kbd(LCA, KEY_0, 0);
    kvm_router_input_t esc = kbd(LCA, KEY_ESC, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    kvm_router_input_t a_held = kbd(0, 4, 0);
    kvm_router_input_t a_and_2 = kbd(LCA, 4, KEY_2);
    kvm_router_input_t shifted = kbd(LCA | 0x02, KEY_2, 0);
    setup_pc(&hub, &f, &p);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &up, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &to2, true) == KVM_HUB_CHORD_HOST_ARMED);
    assert(hub.host_request_target == 2 && hub.target == 0 && !hub.pc_armed && hub.keys[2] == 0);
    /* Held: one request per press, still dropped, the request value stays for the caller. */
    assert(kvm_input_hub_input(&hub, &to2, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(hub.host_request_target == 2);
    assert(kvm_input_hub_input(&hub, &up, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &esc, true) == KVM_HUB_CHORD_HOST_ARMED);
    assert(hub.host_request_target == 0);
    kvm_input_hub_input(&hub, &up, true);
    assert(kvm_input_hub_input(&hub, &to2, true) == KVM_HUB_CHORD_HOST_ARMED);
    kvm_input_hub_input(&hub, &up, true);
    assert(kvm_input_hub_input(&hub, &to0, true) == KVM_HUB_CHORD_HOST_ARMED);
    assert(hub.host_request_target == 0);
    kvm_input_hub_input(&hub, &up, true);
    /* Not chords: another key held first, or an extra modifier. */
    hub.host_request_target = 3;
    kvm_input_hub_input(&hub, &a_held, true);
    assert(kvm_input_hub_input(&hub, &a_and_2, true) == KVM_HUB_DROPPED_HOST_ARMED);
    kvm_input_hub_input(&hub, &up, true);
    assert(kvm_input_hub_input(&hub, &shifted, true) == KVM_HUB_DROPPED_HOST_ARMED);
    kvm_input_hub_input(&hub, &up, true);
    assert(hub.host_request_target == 3);
    assert(f.count == 0 && hub.target == 0);
    /* The router disarms while the chord key is still held: the chord key is still
     * swallowed, never typed into the hub target, until every key is up. */
    assert(kvm_input_hub_input(&hub, &to2, true) == KVM_HUB_CHORD_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED);
    assert(f.count == 0 && hub.target == 0);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(f.count == 2 && on_pc(&f.calls[1], CALL_SEND) && f.calls[1].input.keyboard[2] == 4);
    /* A chord swallowed before the router armed stays swallowed across the arm. */
    setup_pc(&hub, &f, &p);
    kvm_input_hub_set_target(&hub, 1, false);
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_TARGET_CHANGED && hub.target == 2);
    assert(kvm_input_hub_input(&hub, &to2, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(kvm_input_hub_input(&hub, &to2, false) == KVM_HUB_SWALLOWED);
    assert(kvm_input_hub_input(&hub, &up, false) == KVM_HUB_SWALLOWED && !hub.chord_latched);
}

/* Composite mode defaults to This PC as an explicit choice: no stage-2 auto-target. */
static void set_pc_output_starts_on_pc_and_disables_auto_target(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    pc_fake_t p;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    memset(&p, 0, sizeof p);
    p.log = &f;
    p.ready = p.send_ok = p.release_ok = p.arm_ok = true;
    assert(!hub.pc_enabled && hub.pc_live && !hub.pc_armed);
    assert(kvm_input_hub_auto_target(&hub, 0x02, false) && hub.target == 2 && !hub.explicit_target);
    kvm_input_hub_set_pc_output(&hub, pc_output(), &p);
    assert(hub.pc_enabled && hub.target == 0 && hub.explicit_target && hub.pc_live);
    assert(f.count == 0); /* nothing was armed: nothing emitted */
    assert(!kvm_input_hub_auto_target(&hub, 0x07, false) && hub.target == 0);
    assert(!kvm_input_hub_auto_target(&hub, 0x01, false) && hub.target == 0 && f.count == 0);
    /* Bound while a guest holds a key: the guest is released first. */
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 3, false);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    kvm_input_hub_set_pc_output(&hub, pc_output(), &p);
    assert(hub.target == 0 && hub.armed_slot == 0 && hub.explicit_target);
    assert(f.count == 3 && is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 3);
    assert(on_guest(&f.calls[2], CALL_RELEASE, 3));
}

/* Without a PC output target 0 stays the stage-2 stub exactly. */
static void stub_unchanged_without_pc_output(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag = pointer(1);
    kvm_router_input_t chord = kbd(LCA, KEY_2, 0);
    kvm_router_input_t up = kbd(0, 0, 0);
    setup(&hub, &f);
    assert(!hub.pc_enabled && hub.target == 0);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    assert(kvm_input_hub_input(&hub, &drag, false) == KVM_HUB_FORWARDED);
    assert(!hub.pc_armed && hub.armed_slot == 0 && hub.keys[2] == 0 && !hub.dirty);
    assert(kvm_input_hub_flush(&hub, false));
    kvm_input_hub_device_lost(&hub, false);
    assert(kvm_input_hub_yield(&hub, false));
    kvm_input_hub_set_pc_live(&hub, false);
    kvm_input_hub_set_pc_live(&hub, true);
    assert(f.count == 0);
    /* A chord while the router is armed is dropped exactly as in stage 2. */
    kvm_input_hub_input(&hub, &up, false);
    assert(kvm_input_hub_input(&hub, &chord, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(hub.host_request_target == 0 && !hub.chord_latched && hub.target == 0);
    /* Auto-target still follows ready guests. */
    assert(kvm_input_hub_auto_target(&hub, 0x04, false) && hub.target == 3);
    assert(f.count == 0);
}

int main(void)
{
    auto_target_follows_ready_guests_until_explicit();
    keyboard_lost_releases_only_keyboard_state();
    mouse_lost_releases_only_buttons();
    kind_loss_with_nothing_held_releases_the_arm();
    keyboard_lost_resets_chord_tracking();
    failing_ready_target_never_kills_the_keyboard();
    set_target_clears_pending_switch();
    chord_to_selected_guest_releases_held_modifiers();
    esc_on_pc_emits_nothing();
    yield_while_router_armed_abandons_without_emitting();
    device_lost_resets_chord_tracking();
    switch_failure_is_reported_and_retried();
    ctrl_alt_1_selects_guest_1();
    ctrl_alt_0_selects_pc();
    ctrl_alt_esc_releases_all_to_pc();
    chord_with_extra_modifier_is_forwarded();
    right_ctrl_alt_is_not_a_chord();
    chord_key_not_forwarded_after_target_change();
    chord_does_not_fire_when_another_key_is_held();
    chord_with_failed_old_target_stays_swallowed_and_retries();
    all_up_old_target_before_switch();
    chord_ignored_while_router_armed();
    set_target_abandons_dead_old_target();
    chord_abandons_dead_old_target();
    yield_returns_whether_everything_was_released();
    busy_slot_keeps_arm_and_flush_resends();
    dropped_reports_are_resent_as_current_state();
    known_disconnected_slot_is_abandoned();
    retries_never_emit_while_router_armed();
    forwards_to_ready_guest_after_arm();
    drops_when_router_armed();
    drops_when_target_not_ready();
    set_target_sends_all_up_then_arms_new();
    yield_releases_and_clears_arm();
    device_lost_sends_all_up_keyboard_and_zero_mouse();
    target_zero_is_noop_sink();
    invalid_target_rejected();
    yield_before_arm_sends_all_up_then_releases_previous_target();
    failed_all_up_keeps_held_state_and_device_lost_retries();
    failed_release_keeps_arm();
    key_held_while_target_goes_not_ready_is_still_released();
    consumer_held_is_zeroed_on_target_change_and_device_lost();
    guest_targets_use_matching_router_slot_and_zero_emits_nothing();
    stub_unchanged_without_pc_output();
    pc_target_forwards_when_enabled();
    pc_not_ready_marks_dirty_and_flush_resends_want();
    pc_unlive_abandons_without_emitting();
    chord_pc_to_guest_releases_pc_first();
    chord_guest_to_pc_releases_guest_first();
    yield_with_pc_armed_sends_all_up_to_pc();
    suspended_pc_is_released_through_release();
    router_armed_drops_input_and_pc_gets_nothing();
    chord_while_router_armed_returns_host_request();
    set_pc_output_starts_on_pc_and_disables_auto_target();
    return 0;
}
