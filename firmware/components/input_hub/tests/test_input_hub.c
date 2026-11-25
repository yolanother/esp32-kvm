/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the portable input hub with a recording output sink. Cases cover
 * lazy arming, host-armed arbitration, unready targets, target changes, yield,
 * device loss and the local sink without a physical board. */
#include "input_hub.h"
#include <assert.h>
#include <string.h>

#define MAX_CALLS 32

typedef enum { CALL_ARM, CALL_RELEASE, CALL_SEND } call_kind_t;

typedef struct {
    call_kind_t kind;
    uint8_t slot;
    kvm_router_input_t input;
} call_t;

typedef struct {
    bool ready;
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
    record(context, CALL_RELEASE, slot, NULL);
    return true;
}
static bool arm(void *context, uint8_t slot)
{
    record(context, CALL_ARM, slot, NULL);
    return true;
}
static bool send(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    record(context, CALL_SEND, slot, input);
    return true;
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
    assert(kvm_input_hub_set_target(&hub, 2));
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
    kvm_input_hub_set_target(&hub, 1);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_FORWARDED);
    f.count = 0;
    assert(kvm_input_hub_input(&hub, &in, true) == KVM_HUB_DROPPED_HOST_ARMED);
    assert(f.count == 3);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER));
    assert(f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 1);
    assert(hub.armed_slot == 0 && hub.target == 1);
    f.count = 0;
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
    kvm_input_hub_set_target(&hub, 3);
    assert(kvm_input_hub_input(&hub, &in, false) == KVM_HUB_DROPPED_NOT_READY);
    assert(f.count == 0 && hub.target == 3 && hub.armed_slot == 0);
}

static void set_target_sends_all_up_then_arms_new(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    assert(kvm_input_hub_set_target(&hub, 2));
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
    kvm_input_hub_set_target(&hub, 1);
    kvm_input_hub_input(&hub, &in, false);
    f.count = 0;
    kvm_input_hub_yield(&hub);
    assert(f.count == 3 && f.calls[2].kind == CALL_RELEASE && f.calls[2].slot == 1);
    assert(hub.armed_slot == 0 && hub.keys[2] == 0);
    f.count = 0;
    kvm_input_hub_yield(&hub);
    assert(f.count == 0);
}

static void device_lost_sends_all_up_keyboard_and_zero_mouse(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    kvm_router_input_t in = key(4);
    kvm_router_input_t drag;
    setup(&hub, &f);
    kvm_input_hub_set_target(&hub, 1);
    kvm_input_hub_input(&hub, &in, false);
    memset(&drag, 0, sizeof drag);
    drag.kind = KVM_ROUTER_POINTER;
    drag.buttons = 1;
    kvm_input_hub_input(&hub, &drag, false);
    f.count = 0;
    kvm_input_hub_device_lost(&hub);
    assert(f.count == 2);
    assert(is_all_up(&f.calls[0], KVM_ROUTER_KEYBOARD) && f.calls[0].slot == 1);
    assert(is_all_up(&f.calls[1], KVM_ROUTER_POINTER) && f.calls[1].slot == 1);
    assert(hub.target == 1 && hub.buttons == 0 && hub.keys[2] == 0);
    /* Nothing is held now, so a second loss releases the arm. */
    f.count = 0;
    kvm_input_hub_device_lost(&hub);
    assert(f.count == 3 && f.calls[2].kind == CALL_RELEASE && hub.armed_slot == 0);
    f.count = 0;
    kvm_input_hub_device_lost(&hub);
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
    kvm_input_hub_device_lost(&hub);
    kvm_input_hub_yield(&hub);
    assert(kvm_input_hub_set_target(&hub, 0));
    assert(f.count == 0);
}

static void invalid_target_rejected(void)
{
    kvm_input_hub_t hub;
    fake_t f;
    setup(&hub, &f);
    assert(kvm_input_hub_set_target(&hub, 1));
    assert(!kvm_input_hub_set_target(&hub, 4));
    assert(!kvm_input_hub_set_target(&hub, 255));
    assert(hub.target == 1 && f.count == 0);
}

int main(void)
{
    forwards_to_ready_guest_after_arm();
    drops_when_router_armed();
    drops_when_target_not_ready();
    set_target_sends_all_up_then_arms_new();
    yield_releases_and_clears_arm();
    device_lost_sends_all_up_keyboard_and_zero_mouse();
    target_zero_is_noop_sink();
    invalid_target_rejected();
    return 0;
}
