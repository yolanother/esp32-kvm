/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Runs the serialized firmware route gate. It releases the old output before
 * selecting a generation, accepts input only while armed on that generation,
 * and fails local on lease, release, queue, or sustained output failures. */
#include "router.h"
#include <string.h>

static bool valid_session(const kvm_router_t *r, uint64_t session)
{
    return r->session_open && session && session == r->session;
}

static void fail_local(kvm_router_t *r, bool close_session)
{
    uint8_t old = r->slot;
    r->armed = false;
    r->queued = 0;
    r->congested = false;
    if (old && r->output.release && !r->output.release(r->context, old)) {
        if (r->output.disconnect) r->output.disconnect(r->context, old);
    }
    r->slot = 0;
    r->generation++;
    r->fault = true;
    if (close_session) r->session_open = false;
}

static bool expired(kvm_router_t *r, uint64_t now_ms)
{
    if (!r->session_open) return false;
    if (now_ms < r->last_heartbeat_ms ||
        now_ms - r->last_heartbeat_ms >= KVM_ROUTER_LEASE_MS) {
        fail_local(r, true);
        return true;
    }
    return false;
}

static kvm_router_control_result_t *prior(kvm_router_t *r, uint32_t seq,
                                           uint8_t kind, uint64_t argument, uint8_t slot)
{
    for (unsigned i = 0; i < 8; ++i) {
        kvm_router_control_result_t *c = &r->controls[i];
        if (c->valid && c->seq == seq) {
            if (c->kind != kind || c->argument != argument || c->slot != slot) return NULL;
            r->last_result_generation = c->generation;
            return c;
        }
    }
    return NULL;
}

static bool seq_conflict(kvm_router_t *r, uint32_t seq, uint8_t kind, uint64_t argument, uint8_t slot)
{
    for (unsigned i = 0; i < 8; ++i) {
        const kvm_router_control_result_t *c = &r->controls[i];
        if (c->valid && c->seq == seq && (c->kind != kind || c->argument != argument || c->slot != slot))
            return true;
    }
    return false;
}

static bool old_seq(uint32_t candidate, uint32_t newest)
{
    return candidate == newest || (uint32_t)(candidate - newest) >= 0x80000000u;
}

static kvm_router_result_t record(kvm_router_t *r, uint32_t seq, uint8_t kind,
                                  uint64_t argument, uint8_t slot, kvm_router_result_t result)
{
    kvm_router_control_result_t *c = &r->controls[r->next_control++ % 8];
    *c = (kvm_router_control_result_t){seq, kind, r->generation, argument, slot, result, true};
    r->last_result_generation = r->generation;
    r->newest_control_seq = seq;
    r->has_control_seq = true;
    return result;
}

void kvm_router_init(kvm_router_t *r, kvm_router_output_t output, void *context)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->output = output;
    r->context = context;
    r->capacity = 1;
}

bool kvm_router_set_capacity(kvm_router_t *r, uint8_t capacity)
{
    if (!r || r->session_open || r->slot || r->armed || capacity < 1 || capacity > KVM_ROUTER_MAX_SLOTS)
        return false;
    r->capacity = capacity;
    return true;
}

void kvm_router_reset(kvm_router_t *r)
{
    if (!r) return;
    if (r->slot) fail_local(r, true);
    r->slot = 0;
    r->armed = false;
    r->queued = 0;
    r->session_open = false;
    r->congested = false;
    memset(r->controls, 0, sizeof(r->controls));
    r->next_control = 0;
    r->has_control_seq = false;
    r->has_input_seq = false;
}

void kvm_router_emergency_release(kvm_router_t *r)
{
    if (!r) return;
    if (r->slot) fail_local(r, true);
    r->armed = false;
    r->queued = 0;
    r->session_open = false;
    r->fault = true;
}

void kvm_router_session_open(kvm_router_t *r, uint64_t session, uint64_t now_ms)
{
    if (!r) return;
    kvm_router_reset(r);
    if (!session) return;
    r->session = session;
    r->session_open = true;
    r->last_heartbeat_ms = now_ms;
    r->last_host_tick = 0;
    r->has_host_tick = false;
    r->fault = false;
}

kvm_router_result_t kvm_router_heartbeat(kvm_router_t *r, uint64_t session,
                                         uint64_t host_tick, uint64_t now_ms)
{
    if (!r || !valid_session(r, session)) return KVM_ROUTER_STALE_SESSION;
    if (expired(r, now_ms)) return KVM_ROUTER_STALE_SESSION;
    if (r->has_host_tick && host_tick <= r->last_host_tick) return KVM_ROUTER_BAD_PAYLOAD;
    r->last_host_tick = host_tick;
    r->has_host_tick = true;
    r->last_heartbeat_ms = now_ms;
    return KVM_ROUTER_OK;
}

kvm_router_result_t kvm_router_switch(kvm_router_t *r, uint64_t session, uint32_t seq,
                                      uint8_t slot, uint32_t expected, uint32_t requested,
                                      uint64_t now_ms)
{
    uint64_t arg = (uint64_t)expected << 32 | requested;
    if (!r || !valid_session(r, session)) return KVM_ROUTER_STALE_SESSION;
    if (expired(r, now_ms)) return KVM_ROUTER_STALE_SESSION;
    kvm_router_control_result_t *c = prior(r, seq, 0x20, arg, slot);
    if (c) return c->result;
    if (seq_conflict(r, seq, 0x20, arg, slot)) return KVM_ROUTER_BAD_PAYLOAD;
    if (r->has_control_seq && old_seq(seq, r->newest_control_seq)) return KVM_ROUTER_STALE_ROUTE;
    if (expected != r->generation || requested == expected ||
        (uint32_t)(requested - expected) >= 0x80000000u)
        return record(r, seq, 0x20, arg, slot, KVM_ROUTER_STALE_ROUTE);
    if (slot > r->capacity || (slot && (!r->output.ready || !r->output.ready(r->context, slot))))
        return record(r, seq, 0x20, arg, slot, KVM_ROUTER_NOT_READY);
    uint8_t old = r->slot;
    r->armed = false;
    r->queued = 0;
    r->congested = false;
    if (old && (!r->output.release || !r->output.release(r->context, old))) {
        if (r->output.disconnect) r->output.disconnect(r->context, old);
        r->slot = 0;
        r->generation++;
        r->fault = true;
        return record(r, seq, 0x20, arg, slot, KVM_ROUTER_NOT_READY);
    }
    r->generation = requested;
    r->slot = slot;
    r->fault = false;
    return record(r, seq, 0x20, arg, slot, KVM_ROUTER_OK);
}

kvm_router_result_t kvm_router_arm(kvm_router_t *r, uint64_t session, uint32_t seq,
                                   uint8_t slot, uint32_t generation, uint64_t now_ms)
{
    uint64_t arg = (uint64_t)slot << 32 | generation;
    if (!r || !valid_session(r, session)) return KVM_ROUTER_STALE_SESSION;
    if (expired(r, now_ms)) return KVM_ROUTER_STALE_SESSION;
    kvm_router_control_result_t *c = prior(r, seq, 0x22, arg, slot);
    if (c) return c->result;
    if (seq_conflict(r, seq, 0x22, arg, slot)) return KVM_ROUTER_BAD_PAYLOAD;
    if (r->has_control_seq && old_seq(seq, r->newest_control_seq)) return KVM_ROUTER_STALE_ROUTE;
    if (generation != r->generation || slot != r->slot)
        return record(r, seq, 0x22, arg, slot, KVM_ROUTER_STALE_ROUTE);
    if (!slot || !r->output.ready || !r->output.ready(r->context, slot) ||
        !r->output.arm || !r->output.arm(r->context, slot)) {
        fail_local(r, false);
        return record(r, seq, 0x22, arg, slot, KVM_ROUTER_NOT_READY);
    }
    r->armed = true;
    return record(r, seq, 0x22, arg, slot, KVM_ROUTER_OK);
}

kvm_router_result_t kvm_router_release_all(kvm_router_t *r, uint64_t session,
                                           uint32_t seq, uint64_t now_ms)
{
    if (!r || !valid_session(r, session)) return KVM_ROUTER_STALE_SESSION;
    if (expired(r, now_ms)) return KVM_ROUTER_STALE_SESSION;
    kvm_router_control_result_t *c = prior(r, seq, 0x21, 0, 0);
    if (c) return c->result;
    if (seq_conflict(r, seq, 0x21, 0, 0)) return KVM_ROUTER_BAD_PAYLOAD;
    if (r->has_control_seq && old_seq(seq, r->newest_control_seq)) return KVM_ROUTER_STALE_ROUTE;
    uint8_t old = r->slot;
    r->armed = false;
    r->queued = 0;
    r->congested = false;
    bool released = !old || (r->output.release && r->output.release(r->context, old));
    if (!released && r->output.disconnect) r->output.disconnect(r->context, old);
    r->slot = 0;
    r->generation++;
    r->fault = !released;
    return record(r, seq, 0x21, 0, 0, released ? KVM_ROUTER_OK : KVM_ROUTER_NOT_READY);
}

kvm_router_result_t kvm_router_input(kvm_router_t *r, uint64_t session,
                                     uint32_t generation, uint32_t seq,
                                     kvm_router_input_t input, uint64_t now_ms)
{
    if (!r || !valid_session(r, session)) return KVM_ROUTER_STALE_SESSION;
    if (expired(r, now_ms)) return KVM_ROUTER_STALE_SESSION;
    if (generation != r->generation) return KVM_ROUTER_STALE_ROUTE;
    if (!r->armed || !r->slot) return KVM_ROUTER_PAUSED;
    if (r->has_input_seq && old_seq(seq, r->newest_input_seq)) return KVM_ROUTER_STALE_ROUTE;
    if (input.kind > KVM_ROUTER_CONSUMER ||
        (input.kind == KVM_ROUTER_KEYBOARD && input.keyboard[1] != 0) ||
        (input.kind == KVM_ROUTER_POINTER && (input.buttons & ~31u)))
        return KVM_ROUTER_BAD_PAYLOAD;
    if (r->queued == KVM_ROUTER_QUEUE_CAPACITY) {
        fail_local(r, false);
        return KVM_ROUTER_BUSY;
    }
    r->queue[r->queued++] = (kvm_router_entry_t){input, generation, now_ms, seq};
    r->newest_input_seq = seq;
    r->has_input_seq = true;
    return KVM_ROUTER_OK;
}

void kvm_router_tick(kvm_router_t *r, uint64_t now_ms)
{
    if (!r || expired(r, now_ms) || !r->armed) return;
    if (r->congested && (now_ms < r->congestion_since_ms ||
                         now_ms - r->congestion_since_ms >= KVM_ROUTER_CONGESTION_MS)) {
        fail_local(r, false);
        return;
    }
    while (r->queued) {
        kvm_router_entry_t entry = r->queue[0];
        if (entry.generation != r->generation ||
            (entry.input.kind == KVM_ROUTER_POINTER &&
             (now_ms < entry.received_ms || now_ms - entry.received_ms > KVM_ROUTER_MOTION_AGE_MS))) {
            memmove(r->queue, r->queue + 1, --r->queued * sizeof(r->queue[0]));
            continue;
        }
        if (!r->output.send || !r->output.send(r->context, r->slot, &entry.input)) {
            if (!r->congested) {
                r->congested = true;
                r->congestion_since_ms = now_ms;
            }
            return;
        }
        r->congested = false;
        memmove(r->queue, r->queue + 1, --r->queued * sizeof(r->queue[0]));
    }
}
