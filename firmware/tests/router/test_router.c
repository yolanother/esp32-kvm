/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the portable routing actor with a deterministic fake output sink.
 * These cases assert generation fencing, ordered release/arm, bounded queues,
 * heartbeat expiry, and disconnect safety without a physical board. */
#include "router.h"
#include <assert.h>
#include <string.h>

typedef struct {
    bool ready;
    bool send_ok;
    bool release_ok;
    int releases, arms, sends, disconnects;
    uint8_t last_slot;
    kvm_router_input_t last_input;
} fake_t;

static bool ready(void *context, uint8_t slot)
{
    fake_t *f = context;
    return slot == 0 || f->ready;
}
static bool release(void *context, uint8_t slot)
{
    fake_t *f = context;
    f->releases++;
    f->last_slot = slot;
    return f->release_ok;
}
static bool arm(void *context, uint8_t slot)
{
    fake_t *f = context;
    f->arms++;
    f->last_slot = slot;
    return f->ready;
}
static bool send(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    fake_t *f = context;
    if (!f->send_ok) return false;
    f->sends++;
    f->last_slot = slot;
    f->last_input = *input;
    return true;
}
static void disconnect(void *context, uint8_t slot)
{
    fake_t *f = context;
    f->disconnects++;
    f->last_slot = slot;
}
static void setup(kvm_router_t *r, fake_t *f)
{
    kvm_router_output_t out = { ready, release, arm, send, disconnect };
    memset(f, 0, sizeof(*f));
    f->ready = f->send_ok = f->release_ok = true;
    kvm_router_init(r, out, f);
    kvm_router_session_open(r, 7, 0);
}
static void active(kvm_router_t *r, fake_t *f)
{
    setup(r, f);
    assert(kvm_router_switch(r, 7, 1, 1, 0, 1, 1) == KVM_ROUTER_OK);
    assert(kvm_router_arm(r, 7, 2, 1, 1, 2) == KVM_ROUTER_OK);
    assert(r->armed && r->slot == 1);
}
static void switching_and_fencing(void)
{
    kvm_router_t r; fake_t f; setup(&r, &f);
    assert(kvm_router_arm(&r, 7, 1, 1, 0, 0) == KVM_ROUTER_STALE_ROUTE);
    assert(kvm_router_switch(&r, 7, 2, 1, 0, 1, 1) == KVM_ROUTER_OK);
    assert(f.releases == 0 && !r.armed && r.slot == 1 && r.generation == 1);
    assert(kvm_router_switch(&r, 7, 2, 1, 0, 1, 2) == KVM_ROUTER_OK);
    assert(f.releases == 0);
    assert(kvm_router_switch(&r, 7, 3, 1, 0, 2, 2) == KVM_ROUTER_STALE_ROUTE);
    assert(kvm_router_arm(&r, 8, 4, 1, 1, 2) == KVM_ROUTER_STALE_SESSION);
    assert(kvm_router_arm(&r, 7, 4, 1, 1, 2) == KVM_ROUTER_OK);
    assert(f.arms == 1 && r.armed);
    assert(kvm_router_switch(&r, 7, 5, 0, 1, 2, 3) == KVM_ROUTER_OK);
    assert(f.releases == 1 && f.last_slot == 1 && !r.armed);
}
static void inputs_and_release(void)
{
    kvm_router_t r; fake_t f; active(&r, &f);
    kvm_router_input_t key = {0}; key.kind = KVM_ROUTER_KEYBOARD; key.keyboard[2] = 4;
    assert(kvm_router_input(&r, 8, 1, 1, key, 3) == KVM_ROUTER_STALE_SESSION);
    assert(kvm_router_input(&r, 7, 0, 1, key, 3) == KVM_ROUTER_STALE_ROUTE);
    assert(kvm_router_input(&r, 7, 1, 1, key, 3) == KVM_ROUTER_OK);
    kvm_router_tick(&r, 4);
    assert(f.sends == 1 && f.last_input.keyboard[2] == 4);
    assert(kvm_router_release_all(&r, 7, 3, 5) == KVM_ROUTER_OK);
    assert(!r.armed && r.slot == 0 && r.generation == 2);
    assert(kvm_router_release_all(&r, 7, 3, 6) == KVM_ROUTER_OK);
    assert(r.generation == 2);
    assert(kvm_router_input(&r, 7, 1, 2, key, 7) == KVM_ROUTER_STALE_ROUTE);
}
static void lease_and_disconnect(void)
{
    kvm_router_t r; fake_t f; active(&r, &f);
    assert(kvm_router_heartbeat(&r, 7, 100, 100) == KVM_ROUTER_OK);
    assert(kvm_router_heartbeat(&r, 7, 99, 101) == KVM_ROUTER_BAD_PAYLOAD);
    kvm_router_tick(&r, 599);
    assert(r.armed);
    kvm_router_tick(&r, 600);
    assert(!r.armed && !r.session_open && r.slot == 0 && r.generation == 2);
    assert(kvm_router_heartbeat(&r, 7, 101, 601) == KVM_ROUTER_STALE_SESSION);
    active(&r, &f);
    kvm_router_reset(&r);
    assert(!r.armed && r.slot == 0 && !r.session_open);
    assert(kvm_router_arm(&r, 7, 3, 1, 1, 3) == KVM_ROUTER_STALE_SESSION);
}
static void queue_and_backpressure(void)
{
    kvm_router_t r; fake_t f; active(&r, &f);
    kvm_router_input_t motion = {0}; motion.kind = KVM_ROUTER_POINTER; motion.dx = 10;
    assert(kvm_router_input(&r, 7, 1, 3, motion, 3) == KVM_ROUTER_OK);
    kvm_router_tick(&r, 54);
    assert(f.sends == 0 && r.queued == 0);
    f.send_ok = false;
    assert(kvm_router_input(&r, 7, 1, 4, motion, 60) == KVM_ROUTER_OK);
    kvm_router_tick(&r, 60);
    assert(r.queued == 1 && r.armed);
    kvm_router_tick(&r, 160);
    assert(!r.armed && r.slot == 0 && r.queued == 0 && r.fault);
    active(&r, &f);
    for (unsigned i = 0; i < KVM_ROUTER_QUEUE_CAPACITY; ++i)
        assert(kvm_router_input(&r, 7, 1, i + 3, motion, 10) == KVM_ROUTER_OK);
    assert(kvm_router_input(&r, 7, 1, 20, motion, 10) == KVM_ROUTER_BUSY);
    assert(!r.armed && r.slot == 0 && r.queued == 0 && r.fault);
}
static void release_failure(void)
{
    kvm_router_t r; fake_t f; active(&r, &f); f.release_ok = false;
    assert(kvm_router_switch(&r, 7, 3, 0, 1, 2, 3) == KVM_ROUTER_NOT_READY);
    assert(!r.armed && r.slot == 0 && f.disconnects == 1);
}
static void reset_each_stage_and_replay(void)
{
    kvm_router_t r; fake_t f; setup(&r, &f);
    kvm_router_reset(&r);
    assert(!r.session_open && !r.armed && r.slot == 0);
    setup(&r, &f);
    assert(kvm_router_switch(&r, 7, 1, 1, 0, 1, 1) == KVM_ROUTER_OK);
    kvm_router_reset(&r);
    assert(f.releases == 1 && !r.session_open && !r.armed && r.slot == 0);
    active(&r, &f);
    kvm_router_input_t key = {0}; key.kind = KVM_ROUTER_KEYBOARD;
    assert(kvm_router_input(&r, 7, 1, 10, key, 3) == KVM_ROUTER_OK);
    assert(kvm_router_input(&r, 7, 1, 10, key, 3) == KVM_ROUTER_STALE_ROUTE);
    kvm_router_reset(&r);
    assert(r.queued == 0 && !r.armed && !r.session_open);
    kvm_router_session_open(&r, 9, 20);
    assert(kvm_router_input(&r, 7, 1, 11, key, 21) == KVM_ROUTER_STALE_SESSION);
    assert(kvm_router_switch(&r, 9, UINT32_MAX, 1, r.generation,
                             r.generation + 1, 21) == KVM_ROUTER_OK);
    assert(kvm_router_arm(&r, 9, 0, 1, r.generation, 22) == KVM_ROUTER_OK);
    assert(kvm_router_release_all(&r, 9, UINT32_MAX, 23) == KVM_ROUTER_BAD_PAYLOAD);
    assert(kvm_router_release_all(&r, 9, UINT32_MAX - 1, 23) == KVM_ROUTER_STALE_ROUTE);
}
int main(void)
{
    switching_and_fencing();
    inputs_and_release();
    lease_and_disconnect();
    queue_and_backpressure();
    release_failure();
    reset_each_stage_and_replay();
    return 0;
}
