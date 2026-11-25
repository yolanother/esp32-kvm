/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the device-report queue and the worker-side hub glue. The queue
 * is the only object shared with the NimBLE host thread; everything else runs
 * on the USB worker, which serializes the hub with the router. */
#include "transport_hub.h"
#include <string.h>

/* ---- queue ---- */

static void q_lock(kvm_hub_queue_t *q)
{
    if (q->lock) q->lock(q->lock_context);
}

static void q_unlock(kvm_hub_queue_t *q)
{
    if (q->unlock) q->unlock(q->lock_context);
}

static int16_t add_i16(int16_t a, int16_t b)
{
    int32_t sum = (int32_t)a + b;
    return (int16_t)(sum > INT16_MAX ? INT16_MAX : sum < INT16_MIN ? INT16_MIN : sum);
}

static int8_t add_i8(int8_t a, int8_t b)
{
    int16_t sum = (int16_t)(a + b);
    return (int8_t)(sum > INT8_MAX ? INT8_MAX : sum < INT8_MIN ? INT8_MIN : sum);
}

/* Keeps the newest state per kind; pointer motion accumulates across merged reports. */
static void tail_merge(kvm_hub_queue_t *q, const kvm_router_input_t *in)
{
    unsigned kind = (unsigned)in->kind;
    if (kind > KVM_ROUTER_CONSUMER) return;
    if (kind == KVM_ROUTER_POINTER && q->tail_has[kind]) {
        kvm_router_input_t *t = &q->tail_input[kind];
        t->buttons = in->buttons;
        t->dx = add_i16(t->dx, in->dx);
        t->dy = add_i16(t->dy, in->dy);
        t->wheel = add_i8(t->wheel, in->wheel);
        t->pan = add_i8(t->pan, in->pan);
    } else {
        q->tail_input[kind] = *in;
    }
    q->tail_has[kind] = true;
}

static void ring_remove(kvm_hub_queue_t *q, uint8_t index)
{
    uint8_t after = (uint8_t)(q->count - index - 1u);
    memmove(&q->ring[index], &q->ring[index + 1u], after * sizeof q->ring[0]);
    memmove(&q->motion[index], &q->motion[index + 1u], after * sizeof q->motion[0]);
    q->count--;
    if (q->lost && index < q->lost_after) q->lost_after--;
}

static void ring_append(kvm_hub_queue_t *q, const kvm_hub_msg_t *msg, bool motion)
{
    q->ring[q->count] = *msg;
    q->motion[q->count] = motion;
    q->count++;
}

void kvm_hub_queue_init(kvm_hub_queue_t *q, kvm_hub_lock_fn lock, kvm_hub_lock_fn unlock,
                        void *lock_context)
{
    memset(q, 0, sizeof *q);
    q->lock = lock;
    q->unlock = unlock;
    q->lock_context = lock_context;
}

bool kvm_hub_queue_push(kvm_hub_queue_t *q, const kvm_hub_msg_t *msg)
{
    bool motion;
    bool kept = true;
    if (!q || !msg) return false;
    q_lock(q);
    if (msg->type == KVM_HUB_MSG_LOST) {
        /* Delivered after everything queued now and before anything queued later.
         * Tail states are older than the loss, which releases them anyway. */
        q->lost = true;
        q->lost_after = q->count;
        memset(q->tail_has, 0, sizeof q->tail_has);
        q->last_buttons = 0;
        q_unlock(q);
        return true;
    }
    if (msg->type != KVM_HUB_MSG_INPUT || (unsigned)msg->input.kind > KVM_ROUTER_CONSUMER) {
        q_unlock(q);
        return false;
    }
    motion = msg->input.kind == KVM_ROUTER_POINTER && msg->input.buttons == q->last_buttons;
    if (msg->input.kind == KVM_ROUTER_POINTER) q->last_buttons = msg->input.buttons;
    if (q->tail) {
        tail_merge(q, &msg->input);
    } else if (q->count < KVM_HUB_QUEUE_DEPTH) {
        ring_append(q, msg, motion);
    } else if (motion) {
        q->dropped_motion++;
        kept = false;
    } else {
        uint8_t i = 0;
        while (i < q->count && !q->motion[i]) i++;
        if (i < q->count) { /* evict the oldest queued motion */
            ring_remove(q, i);
            q->dropped_motion++;
            ring_append(q, msg, false);
        } else {
            q->tail = true;
            tail_merge(q, &msg->input);
        }
    }
    q_unlock(q);
    return kept;
}

bool kvm_hub_queue_pop(kvm_hub_queue_t *q, kvm_hub_msg_t *out)
{
    static const kvm_router_input_kind_t tail_order[3] = {
        KVM_ROUTER_KEYBOARD, KVM_ROUTER_CONSUMER, KVM_ROUTER_POINTER};
    bool found = false;
    if (!q || !out) return false;
    q_lock(q);
    if (q->lost && q->lost_after == 0) {
        memset(out, 0, sizeof *out);
        out->type = KVM_HUB_MSG_LOST;
        q->lost = false;
        found = true;
    } else if (q->count) {
        *out = q->ring[0];
        ring_remove(q, 0);
        found = true;
    } else if (q->tail) {
        for (unsigned i = 0; i < 3 && !found; i++) {
            unsigned kind = (unsigned)tail_order[i];
            if (!q->tail_has[kind]) continue;
            memset(out, 0, sizeof *out);
            out->type = KVM_HUB_MSG_INPUT;
            out->input = q->tail_input[kind];
            q->tail_has[kind] = false;
            found = true;
        }
        if (!found) q->tail = false; /* fully delivered: back to ordered ring mode */
    }
    q_unlock(q);
    return found;
}

/* ---- worker glue ---- */

/* Forgets the hub's arm and held state without emitting: the slot's channel was
 * released (or dropped) underneath it, so its all-up already happened or cannot.
 * The hub's documented router-armed path is exactly this abandon. */
static void hub_abandon(kvm_hub_worker_t *w)
{
    (void)kvm_input_hub_yield(&w->hub, true);
}

void kvm_hub_worker_init(kvm_hub_worker_t *w, kvm_router_t *router, kvm_router_output_t output,
                         void *context)
{
    memset(w, 0, sizeof *w);
    w->router = router;
    w->output = output;
    w->context = context;
    w->last_result = KVM_HUB_SWALLOWED;
    kvm_input_hub_init(&w->hub, output, context);
}

static bool shim_ready(void *context, uint8_t slot)
{
    kvm_hub_worker_t *w = context;
    return w->output.ready && w->output.ready(w->context, slot);
}

static bool shim_release(void *context, uint8_t slot)
{
    kvm_hub_worker_t *w = context;
    bool ok = w->output.release && w->output.release(w->context, slot);
    /* A channel release disarms the slot whoever armed it. */
    if (w->hub.armed_slot == slot) hub_abandon(w);
    return ok;
}

static bool shim_arm(void *context, uint8_t slot)
{
    kvm_hub_worker_t *w = context;
    /* The transport gate already yielded; this covers any other arm path. */
    if (w->hub.armed_slot != 0 && !kvm_input_hub_yield(&w->hub, false)) return false;
    return w->output.arm && w->output.arm(w->context, slot);
}

static bool shim_send(void *context, uint8_t slot, const kvm_router_input_t *input)
{
    kvm_hub_worker_t *w = context;
    return w->output.send && w->output.send(w->context, slot, input);
}

static void shim_disconnect(void *context, uint8_t slot)
{
    kvm_hub_worker_t *w = context;
    if (w->output.disconnect) w->output.disconnect(w->context, slot);
    if (w->hub.armed_slot == slot) hub_abandon(w);
}

kvm_router_output_t kvm_hub_worker_router_output(void)
{
    kvm_router_output_t output = {shim_ready, shim_release, shim_arm, shim_send, shim_disconnect};
    return output;
}

bool kvm_hub_worker_arm_gate(void *context, uint8_t slot)
{
    kvm_hub_worker_t *w = context;
    (void)slot; /* any hub arm, on any slot, is released before the host arms */
    return kvm_input_hub_yield(&w->hub, w->router->armed);
}

void kvm_hub_worker_message(kvm_hub_worker_t *w, const kvm_hub_msg_t *msg)
{
    bool armed = w->router->armed;
    if (msg->type == KVM_HUB_MSG_LOST) {
        kvm_input_hub_device_lost(&w->hub, armed);
        return;
    }
    if (msg->type != KVM_HUB_MSG_INPUT) return;
    if (w->select_pending && kvm_input_hub_set_target(&w->hub, w->select_target, armed))
        w->select_pending = false;
    w->last_result = kvm_input_hub_input(&w->hub, &msg->input, armed);
    if (w->last_result == KVM_HUB_SWITCH_FAILED) w->switch_failures++;
    if (w->last_result == KVM_HUB_DROPPED_NOT_READY && w->hub.armed_slot != 0 &&
        w->hub.armed_slot == w->hub.target && w->output.ready &&
        w->output.ready(w->context, w->hub.target)) {
        /* A send failed on a ready slot the hub holds: its channel was disarmed
         * underneath it (guest suspend) or is failing. Release the channel (all-up,
         * disarm) and forget the arm so the next report re-arms cleanly. */
        if (w->output.release) (void)w->output.release(w->context, w->hub.armed_slot);
        hub_abandon(w);
    }
    /* A keyboard chord is newer than a pending touch selection. */
    if (w->last_result == KVM_HUB_TARGET_CHANGED || w->last_result == KVM_HUB_SWITCH_FAILED)
        w->select_pending = false;
}

void kvm_hub_worker_drain(kvm_hub_worker_t *w, kvm_hub_queue_t *q)
{
    kvm_hub_msg_t msg;
    unsigned budget = KVM_HUB_QUEUE_DEPTH;
    if (w->router->armed) hub_abandon(w); /* the host owns the output this iteration */
    while (budget-- && kvm_hub_queue_pop(q, &msg)) kvm_hub_worker_message(w, &msg);
}

bool kvm_hub_worker_select(kvm_hub_worker_t *w, uint8_t target)
{
    if (w->router->session_open || target > KVM_INPUT_HUB_MAX_TARGET) return false;
    if (kvm_input_hub_set_target(&w->hub, target, w->router->armed)) {
        w->select_pending = false;
    } else {
        w->select_pending = true;
        w->select_target = target;
    }
    return true;
}

void kvm_hub_worker_slot_lost(kvm_hub_worker_t *w, uint8_t slot)
{
    if (w->hub.armed_slot == 0) return;
    if (slot == w->hub.armed_slot) {
        hub_abandon(w); /* the link is gone, and its channel arm with it */
    } else if (slot == 0) {
        /* Unknown slot: release ours properly rather than assume it dropped. */
        bool pending = w->select_pending;
        kvm_hub_worker_release(w);
        w->select_pending = pending;
    }
}

void kvm_hub_worker_release(kvm_hub_worker_t *w)
{
    w->select_pending = false;
    if (kvm_input_hub_yield(&w->hub, w->router->armed)) return;
    /* The hub could not release a ready slot: release the channel directly (it
     * disarms and sends all-up), disconnecting it when even that is uncertain. */
    if (!w->output.release || !w->output.release(w->context, w->hub.armed_slot)) {
        if (w->output.disconnect) w->output.disconnect(w->context, w->hub.armed_slot);
    }
    hub_abandon(w);
}

uint32_t kvm_hub_central_backoff_ms(uint32_t failures)
{
    uint32_t delay = KVM_HUB_CENTRAL_BACKOFF_MIN_MS;
    while (failures-- && delay < KVM_HUB_CENTRAL_BACKOFF_MAX_MS) delay *= 2u;
    return delay > KVM_HUB_CENTRAL_BACKOFF_MAX_MS ? KVM_HUB_CENTRAL_BACKOFF_MAX_MS : delay;
}
