/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Runs the device-input hub beside the router the way the USB worker does:
 * host frames go through the transport core, device reports through the hub
 * queue, and both share one fake HID output that models a guest channel (an
 * armed channel refuses a second arm; release disarms and sends all-up). It
 * checks arbitration, yield-before-arm, queue overflow and device loss (per
 * kind, through the same central-report conversion the worker uses). */
#include "transport_core.h"
#include "transport_hub.h"
#include "router_hid_bridge.h"
#include "display_pairing.h"
#include "router.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* router_hid_bridge.c links against the NimBLE-host request wrappers; this
 * test only uses its central-report conversion, so the wrappers are inert. */
bool doubtech_hid_request_ready_slot(uint8_t slot) { (void)slot; return false; }
bool doubtech_hid_request_arm_slot(uint8_t slot) { (void)slot; return false; }
bool doubtech_hid_request_release_slot(uint8_t slot) { (void)slot; return false; }
bool doubtech_hid_request_keyboard_slot(uint8_t slot, const uint8_t keys[8]) { (void)slot; (void)keys; return false; }
bool doubtech_hid_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{ (void)slot; (void)buttons; (void)dx; (void)dy; (void)wheel; (void)pan; return false; }
bool doubtech_hid_request_consumer_slot(uint8_t slot, uint16_t usage) { (void)slot; (void)usage; return false; }
esp_err_t doubtech_hid_request_disconnect_slot(uint8_t slot) { (void)slot; return ESP_FAIL; }

/* ---- fake guest channels ---- */
typedef struct { bool ready, armed, fail_send, fail_release, fail_arm; uint8_t keys[8]; uint8_t buttons; uint16_t consumer; } chan_t;
typedef struct { char op; uint8_t slot; kvm_router_input_t in; } op_t;
static chan_t chan[4];
static op_t ops[256];
static int n_ops;
static uint64_t now_ms;
/* Stalled NimBLE host: every request waits out its 20 ms timeout and fails. */
static bool rpc_timeout;
/* Busy request bridge: an earlier request timed out and still holds the host
 * loop, so every request (ready included) is refused at once with no effect
 * on any channel and no disconnect (hid_guest_rpc.c request(), rpc.busy). */
static bool rpc_busy;
static int rpc_calls;

static bool stalled(void)
{
    rpc_calls++;
    if (rpc_busy) return true;
    if (!rpc_timeout) return false;
    now_ms += 20;
    return true;
}
static void log_op(char op, uint8_t slot, const kvm_router_input_t *in)
{
    assert(n_ops < 256);
    ops[n_ops].op = op;
    ops[n_ops].slot = slot;
    memset(&ops[n_ops].in, 0, sizeof ops[n_ops].in);
    if (in) ops[n_ops].in = *in;
    n_ops++;
}
static void all_up(chan_t *c) { memset(c->keys, 0, 8); c->buttons = 0; c->consumer = 0; }
static bool chan_ready(uint8_t slot) { return slot >= 1 && slot <= 3 && chan[slot].ready; }
static bool f_ready(void *ctx, uint8_t slot) { (void)ctx; return !stalled() && chan_ready(slot); }
static bool f_arm(void *ctx, uint8_t slot)
{
    (void)ctx;
    if (stalled()) return false;
    if (!chan_ready(slot) || chan[slot].armed || chan[slot].fail_arm) { log_op('a', slot, NULL); return false; }
    all_up(&chan[slot]);
    chan[slot].armed = true;
    log_op('A', slot, NULL);
    return true;
}
static bool f_release(void *ctx, uint8_t slot)
{
    (void)ctx;
    if (stalled()) return false;
    log_op('R', slot, NULL);
    if (slot < 1 || slot > 3) return false;
    chan[slot].armed = false;
    if (!chan[slot].ready || chan[slot].fail_release) return false;
    all_up(&chan[slot]);
    return true;
}
static bool f_send(void *ctx, uint8_t slot, const kvm_router_input_t *in)
{
    (void)ctx;
    if (stalled()) return false;
    if (!chan_ready(slot) || !chan[slot].armed || chan[slot].fail_send) { log_op('s', slot, in); return false; }
    log_op('S', slot, in);
    if (in->kind == KVM_ROUTER_KEYBOARD) memcpy(chan[slot].keys, in->keyboard, 8);
    else if (in->kind == KVM_ROUTER_POINTER) chan[slot].buttons = in->buttons;
    else chan[slot].consumer = in->consumer;
    return true;
}
/* Disconnecting drops the link: the slot is no longer ready. */
static void f_disconnect(void *ctx, uint8_t slot)
{
    (void)ctx;
    (void)stalled();
    log_op('D', slot, NULL);
    if (slot >= 1 && slot <= 3) { chan[slot].ready = false; chan[slot].armed = false; }
}
static kvm_router_output_t fake_output(void)
{
    kvm_router_output_t o = {f_ready, f_release, f_arm, f_send, f_disconnect};
    return o;
}
static int count_ops(char op, uint8_t slot)
{
    int n = 0;
    for (int i = 0; i < n_ops; i++) n += ops[i].op == op && ops[i].slot == slot;
    return n;
}
static int find_op(char op, uint8_t slot)
{
    for (int i = 0; i < n_ops; i++) if (ops[i].op == op && ops[i].slot == slot) return i;
    return -1;
}

/* ---- host frames (independent encoder, as in test_transport_router.c) ---- */
static uint8_t reply[1024];
static size_t reply_len;
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t v = UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        v ^= p[i];
        for (int b = 0; b < 8; b++) v = (v >> 1) ^ (0x82f63b78u & (0u - (v & 1u)));
    }
    return ~v;
}
static void u32(uint8_t *p, uint32_t n) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static void u64(uint8_t *p, uint64_t n) { u32(p, (uint32_t)n); u32(p + 4, (uint32_t)(n >> 32)); }
static size_t frame(uint8_t *out, uint8_t kind, uint64_t session, uint32_t seq,
                    uint32_t generation, const uint8_t *payload, size_t payload_len)
{
    uint8_t raw[540] = {0};
    raw[0] = 0x56; raw[1] = 0x4b; raw[2] = 1; raw[3] = kind;
    raw[4] = (uint8_t)payload_len; raw[5] = (uint8_t)(payload_len >> 8);
    u64(raw + 8, session); u32(raw + 16, seq); u32(raw + 20, generation);
    if (payload_len) memcpy(raw + 24, payload, payload_len);
    size_t raw_len = 24 + payload_len + 4;
    u32(raw + raw_len - 4, crc(raw, raw_len - 4));
    size_t code_at = 0, at = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < raw_len; i++) {
        if (!raw[i]) { out[code_at] = code; code_at = at++; code = 1; }
        else { out[at++] = raw[i]; if (++code == 0xff) { out[code_at] = code; code_at = at++; code = 1; } }
    }
    out[code_at] = code; out[at++] = 0;
    return at;
}
static size_t decode(uint8_t *out)
{
    size_t at = 0, src = 0;
    while (src + 1 < reply_len) {
        uint8_t code = reply[src++];
        for (uint8_t i = 1; i < code; i++) out[at++] = reply[src++];
        if (code != 0xff && src + 1 < reply_len) out[at++] = 0;
    }
    return at;
}
static void capture(void *context, const uint8_t *p, size_t n)
{ (void)context; assert(n <= sizeof(reply)); memcpy(reply, p, n); reply_len = n; }
static uint64_t clock_ms(void *context) { (void)context; return now_ms; }

/* ---- fixture ---- */
#define SESSION 7u
static kvm_router_t router;
static kvm_hub_worker_t worker;
static kvm_transport_core_t core;
static kvm_hub_queue_t queue;
static uint32_t seq;

static void send_frame(uint8_t kind, uint64_t session, uint32_t s, uint32_t generation,
                       const uint8_t *payload, size_t n)
{
    uint8_t encoded[600];
    size_t len = frame(encoded, kind, session, s, generation, payload, n);
    kvm_transport_core_feed(&core, encoded, len);
}
/* Returns (kind << 8) | error of the last ACK/NACK. */
static unsigned last_result(void)
{
    uint8_t d[600];
    size_t n = decode(d);
    assert(n >= 30);
    return (unsigned)d[3] << 8 | d[29];
}
#define ACK_OK ((unsigned)KVM_MSG_ACK << 8)
#define NACK(e) ((unsigned)KVM_MSG_NACK << 8 | (unsigned)(e))

static void setup(void)
{
    memset(chan, 0, sizeof chan);
    for (int s = 1; s <= 3; s++) chan[s].ready = true;
    n_ops = 0;
    now_ms = 0;
    rpc_timeout = false;
    rpc_busy = false;
    rpc_calls = 0;
    seq = 100;
    kvm_router_init(&router, kvm_hub_worker_router_output(), &worker);
    assert(kvm_router_set_capacity(&router, 3));
    kvm_hub_worker_init(&worker, &router, fake_output(), NULL);
    kvm_hub_worker_set_clock(&worker, clock_ms, NULL);
    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    assert(kvm_transport_core_init(&core, "b", "1", SESSION, capture, NULL));
    kvm_transport_core_bind_router(&core, &router, clock_ms, NULL);
}
static void open_session(void)
{
    const uint8_t hello2[] = {2, 0, 0, 0, 0, 0};
    const uint8_t open[] = {0xa2, 1, 0x61, 'h', 2, 0};
    send_frame(KVM_MSG_HELLO, 0, ++seq, 0, hello2, sizeof hello2);
    send_frame(KVM_MSG_SESSION_OPEN, SESSION, ++seq, 0, open, sizeof open);
    assert(router.session_open && !router.armed);
}
static unsigned host_switch(uint8_t slot)
{
    uint8_t p[9];
    p[0] = slot; u32(p + 1, router.generation); u32(p + 5, router.generation + 1);
    send_frame(KVM_MSG_SWITCH, SESSION, ++seq, router.generation, p, 9);
    return last_result();
}
static unsigned host_arm_seq(uint8_t slot, uint32_t s)
{
    uint8_t p[5];
    p[0] = slot; u32(p + 1, router.generation);
    send_frame(KVM_MSG_ARM, SESSION, s, router.generation, p, 5);
    return last_result();
}
static unsigned host_arm(uint8_t slot) { return host_arm_seq(slot, ++seq); }

static kvm_hub_msg_t key_msg(uint8_t code)
{
    kvm_hub_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = KVM_HUB_MSG_INPUT;
    m.input.kind = KVM_ROUTER_KEYBOARD;
    m.input.keyboard[2] = code;
    return m;
}
static kvm_hub_msg_t pointer_msg(uint8_t buttons, int16_t dx)
{
    kvm_hub_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = KVM_HUB_MSG_INPUT;
    m.input.kind = KVM_ROUTER_POINTER;
    m.input.buttons = buttons;
    m.input.dx = dx;
    return m;
}
/* A loss of one device kind: KVM_ROUTER_KEYBOARD or KVM_ROUTER_POINTER. */
static kvm_hub_msg_t lost_kind_msg(kvm_router_input_kind_t kind)
{
    kvm_hub_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = KVM_HUB_MSG_LOST;
    m.input.kind = kind;
    return m;
}
/* A whole loss (any other kind): everything held is released. */
static kvm_hub_msg_t lost_msg(void) { return lost_kind_msg(KVM_ROUTER_CONSUMER); }
/* The message the worker queues for a central report, as central_input does. */
static kvm_hub_msg_t central_msg(uint8_t kind, bool lost, uint8_t key, uint8_t buttons)
{
    doubtech_hid_central_input_t in;
    kvm_hub_msg_t m;
    memset(&in, 0, sizeof in);
    in.kind = kind;
    in.lost = lost;
    in.keys[2] = key;
    in.buttons = buttons;
    assert(kvm_router_hid_central_msg(&in, &m));
    return m;
}
static void push(kvm_hub_msg_t m) { assert(kvm_hub_queue_push(&queue, &m)); }
static void drain(void) { kvm_hub_worker_drain(&worker, &queue); }
/* Hub on target 1 with key 4 and button 1 held on slot 1. */
static void hub_holding_slot1(void)
{
    assert(kvm_hub_worker_select(&worker, 1));
    push(key_msg(4));
    push(pointer_msg(1, 0));
    drain();
    assert(chan[1].armed && chan[1].keys[2] == 4 && chan[1].buttons == 1);
    assert(worker.hub.armed_slot == 1);
}

/* ---- tests ---- */
static void hub_input_routes_when_no_session(void)
{
    setup();
    assert(!router.session_open);
    assert(kvm_hub_worker_select(&worker, 1) && worker.hub.target == 1);
    push(key_msg(4));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED);
    assert(chan[1].armed && chan[1].keys[2] == 4);
    /* Spec decision 10: a host session that has not armed a guest leaves device
     * input routed to the hub target... */
    open_session();
    push(key_msg(5));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED && chan[1].keys[2] == 5);
    /* ...but touch and PLUS selection belong to the host while a session is open. */
    assert(!kvm_hub_worker_select(&worker, 2) && worker.hub.target == 1);
    /* The host switching its own (unarmed) slot does not move the hub target. */
    assert(host_switch(3) == ACK_OK && router.slot == 3 && !router.armed);
    push(key_msg(6));
    drain();
    assert(chan[1].keys[2] == 6 && worker.hub.target == 1);
}

static void hub_dropped_while_router_armed(void)
{
    setup();
    assert(kvm_hub_worker_select(&worker, 1));
    open_session();
    assert(host_switch(2) == ACK_OK);
    assert(host_arm(2) == ACK_OK && router.armed && router.slot == 2);
    n_ops = 0;
    push(key_msg(4));
    push(pointer_msg(1, 3));
    drain();
    assert(worker.last_result == KVM_HUB_DROPPED_HOST_ARMED);
    assert(n_ops == 0); /* the hub emitted nothing, on any slot */
    assert(!chan[1].armed && worker.hub.armed_slot == 0);
    /* A device loss while armed emits nothing either. */
    push(lost_msg());
    drain();
    assert(n_ops == 0);
    /* The host releasing its route hands device input back to the hub target. */
    send_frame(KVM_MSG_RELEASE_ALL, SESSION, ++seq, router.generation, NULL, 0);
    assert(last_result() == ACK_OK && !router.armed);
    push(key_msg(7));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED && chan[1].keys[2] == 7);
}

static void router_arm_yields_hub(void)
{
    int all_up_kb, all_up_ptr, release, arm;
    /* Same slot: without the yield the channel is still armed by the hub and
     * the router's arm would fail (fail-local, NACK NOT_READY). */
    setup();
    hub_holding_slot1();
    open_session();
    assert(host_switch(1) == ACK_OK);
    n_ops = 0;
    assert(host_arm(1) == ACK_OK);
    assert(router.armed && router.slot == 1 && !router.fault);
    assert(worker.hub.armed_slot == 0 && chan[1].armed);
    all_up_kb = find_op('S', 1);
    assert(all_up_kb >= 0 && ops[all_up_kb].in.kind == KVM_ROUTER_KEYBOARD &&
           ops[all_up_kb].in.keyboard[2] == 0);
    all_up_ptr = all_up_kb + 1;
    assert(ops[all_up_ptr].op == 'S' && ops[all_up_ptr].in.kind == KVM_ROUTER_POINTER &&
           ops[all_up_ptr].in.buttons == 0);
    release = find_op('R', 1);
    arm = find_op('A', 1);
    assert(all_up_kb < release && release < arm); /* hub all-up, hub release, then router arm */
    assert(count_ops('a', 1) == 0);

    /* Different slot: the hub's held keys on slot 1 are released before the
     * router arms slot 2, so nothing stays pressed on slot 1. */
    setup();
    hub_holding_slot1();
    open_session();
    assert(host_switch(2) == ACK_OK);
    n_ops = 0;
    assert(host_arm(2) == ACK_OK && router.armed);
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0 && !chan[1].armed);
    assert(find_op('R', 1) >= 0 && find_op('R', 1) < find_op('A', 2));

    /* Yield fails (ready slot refuses the all-up): the host never sees BUSY.
     * The hub's channel is released directly (all-up, disarm), its arm is
     * abandoned, and the router arms. */
    setup();
    hub_holding_slot1();
    open_session();
    assert(host_switch(1) == ACK_OK);
    chan[1].fail_send = true;
    n_ops = 0;
    assert(host_arm(1) == ACK_OK);
    assert(router.armed && router.slot == 1 && !router.fault);
    assert(worker.hub.armed_slot == 0 && chan[1].keys[2] == 0 && chan[1].buttons == 0);
    assert(find_op('R', 1) >= 0 && find_op('R', 1) < find_op('A', 1));

    /* Yield and the direct release both fail: the hub's slot is disconnected,
     * the hub abandons it, and the router fails only that slot with its
     * existing NOT_READY. Still never BUSY. */
    setup();
    hub_holding_slot1();
    open_session();
    assert(host_switch(1) == ACK_OK);
    chan[1].fail_send = true;
    chan[1].fail_release = true;
    n_ops = 0;
    assert(host_arm(1) == NACK(KVM_ROUTER_NOT_READY));
    assert(!router.armed && worker.hub.armed_slot == 0 && count_ops('D', 1) >= 1);

    /* Same failure on another slot: slot 2 is disconnected, slot 1 arms. */
    setup();
    assert(kvm_hub_worker_select(&worker, 2));
    push(key_msg(4));
    drain();
    assert(chan[2].armed && chan[2].keys[2] == 4);
    open_session();
    assert(host_switch(1) == ACK_OK);
    chan[2].fail_send = true;
    chan[2].fail_release = true;
    assert(host_arm(1) == ACK_OK && router.armed && router.slot == 1);
    assert(count_ops('D', 2) == 1 && worker.hub.armed_slot == 0);
}

static void stale_or_replayed_arm_leaves_hub_alone(void)
{
    uint32_t arm_seq;
    setup();
    assert(kvm_hub_worker_select(&worker, 1)); /* before the session owns selection */
    open_session();
    assert(host_switch(1) == ACK_OK);
    arm_seq = ++seq;
    assert(host_arm_seq(1, arm_seq) == ACK_OK);
    send_frame(KVM_MSG_RELEASE_ALL, SESSION, ++seq, router.generation, NULL, 0);
    assert(last_result() == ACK_OK && !router.armed && router.slot == 0);
    push(key_msg(4));
    drain();
    assert(worker.hub.armed_slot == 1 && chan[1].armed && chan[1].keys[2] == 4);

    /* A replay of the earlier ARM gets its cached answer; the hub is untouched. */
    n_ops = 0;
    {
        uint8_t p[5];
        p[0] = 1; u32(p + 1, router.generation - 1);
        send_frame(KVM_MSG_ARM, SESSION, arm_seq, router.generation - 1, p, 5);
    }
    assert(last_result() == ACK_OK);
    assert(n_ops == 0 && worker.hub.armed_slot == 1 && chan[1].keys[2] == 4);

    /* A stale-generation ARM is refused by the router before the hub yields. */
    {
        uint8_t p[5];
        p[0] = 1; u32(p + 1, router.generation + 5);
        send_frame(KVM_MSG_ARM, SESSION, ++seq, router.generation + 5, p, 5);
    }
    assert(last_result() == NACK(KVM_ROUTER_STALE_ROUTE));
    assert(n_ops == 0 && worker.hub.armed_slot == 1 && chan[1].keys[2] == 4);

    /* A wrong-slot ARM (router selected nothing) is refused the same way. */
    assert(host_arm(2) == NACK(KVM_ROUTER_STALE_ROUTE));
    assert(n_ops == 0 && worker.hub.armed_slot == 1 && chan[1].keys[2] == 4);
}

static void drain_bounded_when_rpc_times_out(void)
{
    uint8_t encoded[600];
    uint8_t heartbeat[8];
    size_t len;
    setup();
    hub_holding_slot1();
    open_session(); /* lease starts at now_ms = 0 */
    for (int i = 0; i < 10; i++) push(key_msg((uint8_t)(i % 2 ? 4 : 5)));
    push(lost_msg());
    push(key_msg(6));
    assert(queue.count == 11 && queue.lost_mask);

    /* The NimBLE host stalls: each request takes its full 20 ms and fails. */
    rpc_timeout = true;
    rpc_calls = 0;
    now_ms = 400;
    u64(heartbeat, 1);
    len = frame(encoded, KVM_MSG_HEARTBEAT, SESSION, ++seq, router.generation, heartbeat, 8);
    kvm_hub_worker_iteration(&worker, &queue, &core, encoded, len);
    /* The heartbeat was serviced before any device work, and the lease holds. */
    assert(router.session_open && router.last_heartbeat_ms == 400);
    /* At most one message's worth of requests ran, then the drain stopped. */
    assert(rpc_calls >= 1 && rpc_calls <= 3 && now_ms - 400 <= 60);
    assert(queue.count >= 9 && queue.lost_mask); /* the rest, and the loss, stay queued */

    /* Further stalled iterations never drop the loss, and keep the session. */
    for (int i = 0; i < 4; i++) {
        u64(heartbeat, (uint64_t)i + 2);
        len = frame(encoded, KVM_MSG_HEARTBEAT, SESSION, ++seq, router.generation, heartbeat, 8);
        kvm_hub_worker_iteration(&worker, &queue, &core, encoded, len);
        assert(router.session_open);
    }
    assert(queue.lost_mask);

    /* Healthy again: everything drains, the loss included, nothing stays held. */
    rpc_timeout = false;
    for (int i = 0; i < 4; i++) kvm_hub_worker_iteration(&worker, &queue, &core, NULL, 0);
    assert(!kvm_hub_queue_pending(&queue));
    assert(chan[1].keys[2] == 6 || chan[1].keys[2] == 0);
    push(lost_msg());
    kvm_hub_worker_iteration(&worker, &queue, &core, NULL, 0);
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0);
}

static void drain_stops_at_first_failed_request(void)
{
    setup();
    hub_holding_slot1();
    for (int i = 0; i < 6; i++) push(key_msg((uint8_t)(i % 2 ? 4 : 5)));
    chan[1].fail_send = true; /* fails at once, no time passes */
    drain();
    assert(queue.count == 5); /* stopped after the first failed send */
    chan[1].fail_send = false;
    drain();
    drain();
    assert(!kvm_hub_queue_pending(&queue) && chan[1].armed && chan[1].keys[2] == 4);
}

static void router_release_rearms_hub(void)
{
    setup();
    hub_holding_slot1();
    open_session();
    assert(host_switch(1) == ACK_OK && !router.armed);
    /* The host moves its unarmed route off slot 1: the router releases slot 1,
     * which disarms the hub's channel; the hub must forget its arm. */
    assert(host_switch(2) == ACK_OK);
    assert(!chan[1].armed && worker.hub.armed_slot == 0);
    push(key_msg(9));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED && chan[1].armed && chan[1].keys[2] == 9);
}

static void queue_overflow_keeps_release(void)
{
    kvm_hub_msg_t m;
    int motions = 0, n = 0;
    /* a. press, 15 motions, release: the release evicts the oldest motion. */
    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    push(key_msg(4));
    for (int i = 0; i < 15; i++) push(pointer_msg(0, 1));
    assert(queue.count == KVM_HUB_QUEUE_DEPTH);
    m = pointer_msg(0, 1);
    assert(!kvm_hub_queue_push(&queue, &m)); /* pure motion is what overflow drops */
    push(key_msg(0));
    while (kvm_hub_queue_pop(&queue, &m)) {
        if (n == 0) assert(m.input.kind == KVM_ROUTER_KEYBOARD && m.input.keyboard[2] == 4);
        motions += m.input.kind == KVM_ROUTER_POINTER;
        n++;
    }
    assert(motions == 14 && m.input.kind == KVM_ROUTER_KEYBOARD && m.input.keyboard[2] == 0);

    /* b. ring full of key states: later states keep the newest per kind, in order. */
    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    for (int i = 0; i < 16; i++) push(key_msg((uint8_t)(i % 2 ? 0 : 4)));
    push(key_msg(5));
    push(pointer_msg(1, 2)); /* button press in tail */
    push(pointer_msg(1, 3)); /* motion accumulates */
    push(pointer_msg(0, 0)); /* button release */
    push(key_msg(0));        /* final key release */
    n = 0;
    kvm_router_input_t last_kb = {0}, last_ptr = {0};
    last_ptr.buttons = 0xff;
    while (kvm_hub_queue_pop(&queue, &m)) {
        n++;
        if (m.input.kind == KVM_ROUTER_KEYBOARD) last_kb = m.input;
        else last_ptr = m.input;
    }
    assert(n == 18 && last_kb.keyboard[2] == 0 && last_ptr.buttons == 0 && last_ptr.dx == 5);
    assert(!queue.tail);

    /* c. through the worker: a release queued behind a full ring still reaches the guest. */
    setup();
    assert(kvm_hub_worker_select(&worker, 1));
    for (int i = 0; i < 16; i++) push(key_msg((uint8_t)(i % 2 ? 4 : 5)));
    push(key_msg(0));
    drain(); /* bounded: at most one ring's worth per call */
    drain();
    assert(chan[1].keys[2] == 0);
}

static void device_lost_sends_all_up(void)
{
    kvm_hub_msg_t m;
    setup();
    hub_holding_slot1();
    n_ops = 0;
    push(lost_msg());
    drain();
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0);
    assert(count_ops('S', 1) >= 2);
    assert(ops[0].in.kind == KVM_ROUTER_KEYBOARD && ops[1].in.kind == KVM_ROUTER_POINTER);

    /* A loss is never dropped, even behind a full ring, and keeps its place. */
    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    push(key_msg(4));
    for (int i = 0; i < 15; i++) push(pointer_msg(0, 1));
    push(lost_msg());
    push(key_msg(0)); /* post-loss: evicts a motion queued before the loss */
    int n = 0, lost_at = -1;
    while (kvm_hub_queue_pop(&queue, &m)) {
        if (m.type == KVM_HUB_MSG_LOST) lost_at = n;
        n++;
    }
    assert(n == 17 && lost_at == 15 && m.type == KVM_HUB_MSG_INPUT);

    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    for (int i = 0; i < 16; i++) push(key_msg(4));
    push(key_msg(6)); /* tail state before the loss is superseded by it */
    push(lost_msg());
    push(key_msg(7)); /* tail state after the loss */
    n = 0; lost_at = -1;
    while (kvm_hub_queue_pop(&queue, &m)) {
        if (m.type == KVM_HUB_MSG_LOST) lost_at = n;
        n++;
    }
    assert(n == 18 && lost_at == 16 && m.input.keyboard[2] == 7);

    /* End to end: a press then a loss behind a full ring leaves nothing held. */
    setup();
    assert(kvm_hub_worker_select(&worker, 1));
    for (int i = 0; i < 16; i++) push(key_msg(4));
    push(lost_msg());
    drain();
    drain();
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0);
}

/* A paired keyboard drops (asleep, out of range) while a key is held: the
 * central's lost report reaches the hub through the same conversion and queue
 * the worker uses, and only what that device could hold is released. */
static void central_disconnect_releases_held_input_per_kind(void)
{
    kvm_hub_msg_t m;
    setup();
    assert(kvm_hub_worker_select(&worker, 1));
    push(central_msg(DOUBTECH_HID_CENTRAL_KEYBOARD, false, 4, 0));
    push(central_msg(DOUBTECH_HID_CENTRAL_MOUSE, false, 0, 1));
    drain();
    assert(chan[1].keys[2] == 4 && chan[1].buttons == 1 && worker.hub.armed_slot == 1);
    n_ops = 0;
    push(central_msg(DOUBTECH_HID_CENTRAL_KEYBOARD, true, 0, 0));
    drain();
    /* All keys up on the target; the still-connected mouse keeps its drag. */
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 1 && chan[1].armed);
    assert(count_ops('S', 1) == 1 && ops[0].in.kind == KVM_ROUTER_KEYBOARD);
    /* The mouse drops too: its button is released. */
    push(central_msg(DOUBTECH_HID_CENTRAL_MOUSE, true, 0, 0));
    drain();
    assert(chan[1].buttons == 0);
    /* The keyboard reconnects and types again on the same target. */
    push(central_msg(DOUBTECH_HID_CENTRAL_KEYBOARD, false, 9, 0));
    drain();
    assert(chan[1].keys[2] == 9);

    /* Both losses queued behind a full ring: each is delivered once, in its place. */
    kvm_hub_queue_init(&queue, NULL, NULL, NULL);
    for (int i = 0; i < 4; i++) push(key_msg(4));
    push(lost_kind_msg(KVM_ROUTER_POINTER));
    push(key_msg(5));
    push(lost_kind_msg(KVM_ROUTER_KEYBOARD));
    push(pointer_msg(2, 0));
    int n = 0, kb_at = -1, ptr_at = -1, losses = 0;
    while (kvm_hub_queue_pop(&queue, &m)) {
        if (m.type == KVM_HUB_MSG_LOST) {
            losses++;
            if (m.input.kind == KVM_ROUTER_KEYBOARD) kb_at = n;
            if (m.input.kind == KVM_ROUTER_POINTER) ptr_at = n;
        }
        n++;
    }
    assert(n == 8 && losses == 2 && ptr_at == 4 && kb_at == 6);

    /* A keyboard lost behind a full ring of key presses still ends all-up,
     * and a button press queued before the loss is not discarded by it. */
    setup();
    assert(kvm_hub_worker_select(&worker, 1));
    push(pointer_msg(1, 0));
    for (int i = 0; i < 15; i++) push(key_msg(4));
    push(key_msg(6));
    push(central_msg(DOUBTECH_HID_CENTRAL_KEYBOARD, true, 0, 0));
    drain();
    drain();
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 1);

    /* While the host route is armed a loss emits nothing on any slot. */
    setup();
    open_session();
    assert(host_switch(2) == ACK_OK && host_arm(2) == ACK_OK);
    n_ops = 0;
    push(central_msg(DOUBTECH_HID_CENTRAL_KEYBOARD, true, 0, 0));
    drain();
    assert(n_ops == 0);
}

static void select_retries_on_later_input(void)
{
    setup();
    hub_holding_slot1();
    chan[1].fail_send = true; /* old target refuses the all-up while ready */
    assert(kvm_hub_worker_select(&worker, 2));
    assert(worker.hub.target == 1 && worker.select_pending);
    chan[1].fail_send = false;
    push(key_msg(5));
    drain();
    assert(!worker.select_pending && worker.hub.target == 2);
    assert(chan[1].keys[2] == 0 && !chan[1].armed && chan[2].keys[2] == 5);
}

static void switch_failed_is_counted_and_retried(void)
{
    kvm_hub_msg_t chord = key_msg(0x1f); /* Left Ctrl + Left Alt + 2 */
    chord.input.keyboard[0] = 0x05;
    setup();
    hub_holding_slot1();
    push(key_msg(0)); /* a chord key must be newly pressed */
    drain();
    chan[1].fail_send = true;
    push(chord);
    drain();
    assert(worker.last_result == KVM_HUB_SWITCH_FAILED && worker.switch_failures == 1);
    chan[1].fail_send = false;
    push(chord); /* still held: the latched chord retries the switch */
    drain();
    assert(worker.last_result == KVM_HUB_TARGET_CHANGED && worker.hub.target == 2);
}

static void slot_lost_and_emergency(void)
{
    setup();
    hub_holding_slot1();
    chan[1].armed = false; /* the guest dropped and came back unarmed */
    kvm_hub_worker_slot_lost(&worker, 1);
    assert(worker.hub.armed_slot == 0);
    push(key_msg(8));
    drain();
    assert(chan[1].armed && chan[1].keys[2] == 8);
    kvm_hub_worker_release(&worker);
    assert(!chan[1].armed && chan[1].keys[2] == 0 && worker.hub.armed_slot == 0);

    /* A disconnect for an unknown slot releases with all-up instead of assuming. */
    setup();
    hub_holding_slot1();
    kvm_hub_worker_slot_lost(&worker, 2);
    assert(worker.hub.armed_slot == 1 && chan[1].keys[2] == 4);
    kvm_hub_worker_slot_lost(&worker, 0);
    assert(worker.hub.armed_slot == 0 && !chan[1].armed && chan[1].keys[2] == 0);

    /* The guest disarmed the channel on its own (HID control-point suspend):
     * the dropped report resets the channel so the next report re-arms. */
    setup();
    hub_holding_slot1();
    chan[1].armed = false;
    push(key_msg(5));
    drain();
    assert(worker.last_result == KVM_HUB_DROPPED_NOT_READY && worker.hub.armed_slot == 0);
    push(key_msg(6));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED && chan[1].armed && chan[1].keys[2] == 6);
}

/* ready() false only because the request bridge is busy (an earlier request
 * timed out and still holds the host loop; no disconnect): the guest channel is
 * still armed and still holds the keys. The hub must keep its arm and held
 * state instead of forgetting them, resend the dropped release once the bridge
 * answers, and a later host ARM of that slot must succeed with nothing held. */
static void busy_bridge_keeps_hub_arm_and_host_arm_succeeds(void)
{
    setup();
    hub_holding_slot1();
    rpc_busy = true;
    push(key_msg(0));        /* key-up: dropped, the bridge refused */
    push(pointer_msg(0, 0)); /* button-up: dropped */
    push(lost_msg());        /* and both devices are lost */
    drain();
    drain();
    assert(chan[1].armed && chan[1].keys[2] == 4); /* the channel saw none of it */
    assert(worker.hub.armed_slot == 1 && worker.hub.dirty); /* the hub still knows it holds it */
    rpc_busy = false;
    /* The host arms that slot before the hub's next drain: the hub yields first. */
    open_session();
    assert(host_switch(1) == ACK_OK);
    assert(host_arm(1) == ACK_OK && router.armed && !router.fault);
    assert(chan[1].armed && chan[1].keys[2] == 0 && chan[1].buttons == 0);
    assert(worker.hub.armed_slot == 0);
    drain(); /* the queued device reports are dropped: the host owns the output */
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0 && router.armed);

    /* The busy spell ends with no host ARM: the next drain resends the held
     * state, then delivers the queued loss. Nothing stays pressed. */
    setup();
    hub_holding_slot1();
    rpc_busy = true;
    push(key_msg(0));
    push(lost_msg());
    drain();
    assert(chan[1].keys[2] == 4 && worker.hub.armed_slot == 1);
    rpc_busy = false;
    drain();
    assert(chan[1].keys[2] == 0 && chan[1].buttons == 0 && !kvm_hub_queue_pending(&queue));
    assert(!worker.hub.dirty && worker.hub.keys[2] == 0 && worker.hub.buttons == 0);
}

/* The router's arm finds the channel already armed by an arm nobody tracks:
 * the router output releases the slot and retries the arm exactly once, so the
 * host ARM is not refused (a NACK ends the host session). */
static void host_arm_recovers_a_stale_channel_arm(void)
{
    setup();
    open_session();
    assert(host_switch(1) == ACK_OK);
    chan[1].armed = true;
    chan[1].keys[2] = 4;
    n_ops = 0;
    assert(host_arm(1) == ACK_OK && router.armed && !router.fault);
    assert(chan[1].armed && chan[1].keys[2] == 0);
    assert(count_ops('a', 1) == 1 && count_ops('A', 1) == 1);
    assert(find_op('a', 1) < find_op('R', 1) && find_op('R', 1) < find_op('A', 1));
    /* A slot that cannot arm at all still fails, after one retry only. */
    setup();
    open_session();
    assert(host_switch(2) == ACK_OK);
    chan[2].fail_arm = true;
    n_ops = 0;
    assert(host_arm(2) == NACK(KVM_ROUTER_NOT_READY));
    assert(count_ops('a', 2) == 2 && count_ops('A', 2) == 0);
}

/* A slot known disconnected (slot sample, DISCONNECTED event) is still
 * abandoned without any request; a busy spell keeps later reports queued in
 * order, so a chord typed during it still switches once the bridge answers. */
static void disconnected_slot_still_abandons(void)
{
    kvm_hub_msg_t chord = key_msg(0x1f); /* Left Ctrl + Left Alt + 2 */
    chord.input.keyboard[0] = 0x05;
    setup();
    hub_holding_slot1();
    n_ops = 0;
    chan[1].ready = false;
    chan[1].armed = false;                   /* the link dropped */
    kvm_hub_worker_set_live(&worker, 0x06);  /* and the slot sample says so */
    assert(worker.hub.armed_slot == 0 && n_ops == 0);
    push(lost_msg());
    drain();
    assert(n_ops == 0 && worker.hub.keys[2] == 0);

    setup();
    hub_holding_slot1();
    n_ops = 0;
    kvm_hub_worker_slot_lost(&worker, 1);    /* DISCONNECTED event */
    assert(worker.hub.armed_slot == 0 && !(worker.hub.live_mask & 1u) && n_ops == 0);
    chan[1].armed = false;                   /* it reconnects with a fresh channel */
    kvm_hub_worker_set_live(&worker, 0x07);
    push(key_msg(9));
    drain();
    assert(chan[1].armed && chan[1].keys[2] == 9 && worker.hub.armed_slot == 1);

    setup();
    hub_holding_slot1();
    rpc_busy = true;
    push(key_msg(0));
    push(chord);
    push(key_msg(0));
    drain();
    drain();
    assert(queue.count == 2 && worker.hub.target == 1); /* waiting, not dropped */
    rpc_busy = false;
    drain();
    assert(chan[1].keys[2] == 0 && !chan[1].armed && worker.hub.target == 2);
    assert(!kvm_hub_queue_pending(&queue));
}

/* After boot (nothing chosen yet) the owner types with no chord: input goes
 * to the first ready guest, not to the This PC stub. Guests coming and going
 * move it only until an explicit choice, which then holds (no persistence). */
static void boot_default_target_is_first_ready_guest(void)
{
    setup();
    assert(worker.hub.target == 0);
    kvm_hub_worker_auto_target(&worker, 0x00); /* no guest yet: nothing to pick */
    assert(worker.hub.target == 0);
    kvm_hub_worker_auto_target(&worker, 0x01); /* guest 1 reconnects after boot */
    assert(worker.hub.target == 1);
    push(key_msg(4));
    drain();
    assert(worker.last_result == KVM_HUB_FORWARDED && chan[1].armed && chan[1].keys[2] == 4);
    /* Guest 1 drops (DISCONNECTED, then the slot sample): guest 2 takes over. */
    chan[1].ready = false;
    chan[1].armed = false;
    kvm_hub_worker_slot_lost(&worker, 1);
    kvm_hub_worker_auto_target(&worker, 0x02);
    assert(worker.hub.target == 2);
    push(key_msg(5));
    drain();
    assert(chan[2].keys[2] == 5);
    /* An explicit touch choice of This PC is remembered until reboot. */
    assert(kvm_hub_worker_select(&worker, 0) && worker.hub.target == 0);
    kvm_hub_worker_auto_target(&worker, 0x07);
    assert(worker.hub.target == 0 && !chan[2].armed && chan[2].keys[2] == 0);
    /* The same rule with a host session open but unarmed (the app is running). */
    setup();
    open_session();
    kvm_hub_worker_auto_target(&worker, 0x04);
    assert(worker.hub.target == 3);
    push(key_msg(6));
    drain();
    assert(chan[3].keys[2] == 6);
}

/* ---- input-device pairing beside the router and guest pairing ---- */
static kvm_display_input_pairing_t input_pairing;
static int central_begin_calls, central_cancel_calls, guest_begin_calls;
static kvm_display_input_op_t central_begin_result;
static bool central_cancel_ok;
static kvm_display_input_op_t central_begin(void *c)
{ (void)c; central_begin_calls++; return central_begin_result; }
static kvm_display_input_op_t central_select(void *c, uint8_t index)
{ (void)c; (void)index; return KVM_DISPLAY_INPUT_OP_OK; }
static bool central_cancel(void *c) { (void)c; central_cancel_calls++; return central_cancel_ok; }
/* The worker's PC-app guest PAIR_BEGIN: refused while the input window is open. */
static bool guest_pair_begin(void *c)
{
    (void)c;
    if (kvm_display_input_pairing_blocks_guest(&input_pairing)) {
        kvm_display_input_pairing_guest_refused(&input_pairing, now_ms);
        return false;
    }
    guest_begin_calls++;
    return true;
}
static unsigned host_pair_begin(void)
{
    const uint8_t window[2] = {60, 0};
    send_frame(KVM_MSG_PAIR_BEGIN, SESSION, ++seq, router.generation, window, 2);
    return last_result();
}
static void input_setup(void)
{
    setup();
    central_begin_calls = central_cancel_calls = guest_begin_calls = 0;
    central_begin_result = KVM_DISPLAY_INPUT_OP_OK;
    central_cancel_ok = true;
    kvm_display_input_pairing_init(&input_pairing,
        (kvm_display_input_ops_t){central_begin, central_select, central_cancel, NULL});
    kvm_transport_core_bind_pairing(&core,
        (kvm_transport_pairing_ops_t){.begin = guest_pair_begin}, NULL);
}

static const kvm_display_input_request_t input_begin = {KVM_DISPLAY_INPUT_PAIR_BEGIN, 0, 0, 0};

/* The owner's PC app is always connected: an open but unarmed session must
 * not stop input pairing (the gate is router.armed, as the worker passes). */
static void input_pairing_allowed_with_unarmed_session(void)
{
    input_setup();
    open_session();
    assert(router.session_open && !router.armed);
    assert(kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                             router.armed, false));
    assert(central_begin_calls == 1 && input_pairing.state == KVM_DISPLAY_INPUT_SCANNING);
    /* The worker's tick leaves the window alone while the session stays unarmed. */
    kvm_display_input_pairing_tick(&input_pairing, now_ms + 20, router.armed);
    assert(central_cancel_calls == 0 && input_pairing.state == KVM_DISPLAY_INPUT_SCANNING);
    /* Device input routing is untouched by any of it. */
    assert(n_ops == 0 && !router.armed && worker.hub.armed_slot == 0);
}

static void guest_pairing_refused_while_input_window_open(void)
{
    input_setup();
    open_session();
    assert(kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                             router.armed, false));
    /* The app asks for a guest window during the input window: BUSY, and the
     * device shows why. */
    assert(host_pair_begin() == NACK(KVM_ROUTER_BUSY) && guest_begin_calls == 0);
    {
        kvm_display_status_t s;
        memset(&s, 0, sizeof s);
        kvm_display_input_pairing_status(&input_pairing, now_ms, &s);
        assert(s.input_pair_guest_busy && s.input_pair_state == KVM_DISPLAY_INPUT_SCANNING);
    }
    /* Still refused while the result shows; allowed once it is dismissed.
     * (Only the pairing clock moves: the host lease stays fresh.) */
    kvm_display_input_pairing_tick(&input_pairing, now_ms + KVM_DISPLAY_INPUT_WINDOW_MS, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_RESULT && central_cancel_calls == 1);
    assert(host_pair_begin() == NACK(KVM_ROUTER_BUSY) && guest_begin_calls == 0);
    kvm_display_input_pairing_tick(&input_pairing,
        now_ms + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_RESULT_MS, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_CLOSED);
    assert(host_pair_begin() == ACK_OK && guest_begin_calls == 1);
    /* And the other way: an open guest window refuses the input window. */
    input_setup();
    assert(!kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                              router.armed, true));
    assert(central_begin_calls == 0 && input_pairing.reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
           input_pairing.busy_cause == KVM_DISPLAY_INPUT_BUSY_GUEST);
}

static void input_pairing_refused_and_ended_when_router_armed(void)
{
    /* Armed before BEGIN: refused with the host cause, central untouched. */
    input_setup();
    open_session();
    assert(host_switch(1) == ACK_OK && host_arm(1) == ACK_OK && router.armed);
    assert(!kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                              router.armed, false));
    assert(central_begin_calls == 0 && input_pairing.reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
           input_pairing.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST);
    /* Armed mid-window: the next tick cancels the central and shows BUSY. */
    input_setup();
    open_session();
    assert(kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                             router.armed, false));
    assert(host_switch(2) == ACK_OK && host_arm(2) == ACK_OK && router.armed);
    kvm_display_input_pairing_tick(&input_pairing, now_ms + 20, router.armed);
    assert(central_cancel_calls == 1 && input_pairing.state == KVM_DISPLAY_INPUT_RESULT &&
           input_pairing.reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
           input_pairing.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST);
    assert(router.armed && router.slot == 2); /* the host route is untouched */
}

static void begin_rpc_timeout_holds_guest_gate(void)
{
    uint64_t t;
    input_setup();
    open_session();
    central_begin_result = KVM_DISPLAY_INPUT_OP_UNKNOWN; /* request() hit ESP_ERR_TIMEOUT */
    central_cancel_ok = false;                           /* the abandoned begin still holds the loop */
    assert(!kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                              router.armed, false));
    assert(central_begin_calls == 1 && input_pairing.stopping && central_cancel_calls == 1);
    /* The result screen shows BUSY, but the window is not reported closed and
     * guest pairing stays refused well past the normal 4 s result. */
    for (t = now_ms + 500; t < now_ms + KVM_DISPLAY_INPUT_WINDOW_MS; t += 500)
        kvm_display_input_pairing_tick(&input_pairing, t, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_RESULT && input_pairing.stopping);
    assert(host_pair_begin() == NACK(KVM_ROUTER_BUSY) && guest_begin_calls == 0);
    assert(central_cancel_calls >= 25); /* retried, about once a second */
    /* Until the central's own timer has surely ended the window. */
    kvm_display_input_pairing_tick(&input_pairing,
        now_ms + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_CLOSED);
    assert(host_pair_begin() == ACK_OK && guest_begin_calls == 1);

    /* A cancel the central confirms (it ran after the abandoned begin) releases early. */
    input_setup();
    open_session();
    central_begin_result = KVM_DISPLAY_INPUT_OP_UNKNOWN;
    central_cancel_ok = false;
    assert(!kvm_display_input_pairing_request(&input_pairing, input_begin, now_ms,
                                              router.armed, false));
    assert(host_pair_begin() == NACK(KVM_ROUTER_BUSY));
    central_cancel_ok = true;
    kvm_display_input_pairing_tick(&input_pairing, now_ms + KVM_DISPLAY_INPUT_CANCEL_RETRY_MS,
                                   router.armed);
    assert(!input_pairing.stopping && central_cancel_calls == 2);
    kvm_display_input_pairing_tick(&input_pairing, now_ms + KVM_DISPLAY_INPUT_RESULT_MS, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_CLOSED);
    assert(host_pair_begin() == ACK_OK && guest_begin_calls == 1);
}

/* Forget from the device list, gated the way the worker gates it: allowed
 * with an open, unarmed session; refused (central untouched) once the host
 * arms a guest, and the open list closes on the next tick. */
static int central_forget_calls;
static kvm_display_input_op_t central_forget(void *c, uint8_t kind)
{ (void)c; (void)kind; central_forget_calls++; return KVM_DISPLAY_INPUT_OP_OK; }
static void forget_gated_by_the_host_route(void)
{
    kvm_display_input_request_t open = {KVM_DISPLAY_INPUT_DEVICES_OPEN, 0, 0, 0};
    kvm_display_input_request_t forget = {KVM_DISPLAY_INPUT_FORGET, 0, 0, 0};
    input_setup();
    central_forget_calls = 0;
    input_pairing.ops.forget = central_forget;
    kvm_display_input_pairing_device(&input_pairing, KVM_DISPLAY_INPUT_KEYBOARD, true, "K380");
    open_session();
    assert(kvm_display_input_pairing_request(&input_pairing, open, now_ms, router.armed, false));
    forget.window = input_pairing.window;
    assert(kvm_display_input_pairing_request(&input_pairing, forget, now_ms, router.armed, false));
    assert(kvm_display_input_pairing_request(&input_pairing, forget, now_ms + 100, router.armed, false));
    assert(central_forget_calls == 1 && !(input_pairing.stored_mask & (1u << KVM_DISPLAY_INPUT_KEYBOARD)));
    /* The PC app arms a guest while the list is open: the list closes as BUSY. */
    kvm_display_input_pairing_device(&input_pairing, KVM_DISPLAY_INPUT_KEYBOARD, true, "K380");
    assert(host_switch(1) == ACK_OK && host_arm(1) == ACK_OK && router.armed);
    assert(!kvm_display_input_pairing_request(&input_pairing, forget, now_ms + 200, router.armed, false));
    assert(central_forget_calls == 1 && input_pairing.state == KVM_DISPLAY_INPUT_RESULT &&
           input_pairing.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST);
    input_setup();
    open_session();
    assert(kvm_display_input_pairing_request(&input_pairing, open, now_ms, router.armed, false));
    assert(host_switch(2) == ACK_OK && host_arm(2) == ACK_OK);
    kvm_display_input_pairing_tick(&input_pairing, now_ms + 20, router.armed);
    assert(input_pairing.state == KVM_DISPLAY_INPUT_RESULT && central_cancel_calls == 0);
    assert(router.armed && router.slot == 2);
}

static void central_backoff_is_bounded(void)
{
    assert(kvm_hub_central_backoff_ms(0) == KVM_HUB_CENTRAL_BACKOFF_MIN_MS);
    assert(kvm_hub_central_backoff_ms(1) == 2 * KVM_HUB_CENTRAL_BACKOFF_MIN_MS);
    assert(kvm_hub_central_backoff_ms(7) == KVM_HUB_CENTRAL_BACKOFF_MAX_MS);
    assert(kvm_hub_central_backoff_ms(4000000000u) == KVM_HUB_CENTRAL_BACKOFF_MAX_MS);
}

int main(void)
{
    hub_input_routes_when_no_session();
    hub_dropped_while_router_armed();
    router_arm_yields_hub();
    stale_or_replayed_arm_leaves_hub_alone();
    drain_bounded_when_rpc_times_out();
    drain_stops_at_first_failed_request();
    router_release_rearms_hub();
    queue_overflow_keeps_release();
    device_lost_sends_all_up();
    central_disconnect_releases_held_input_per_kind();
    select_retries_on_later_input();
    switch_failed_is_counted_and_retried();
    slot_lost_and_emergency();
    busy_bridge_keeps_hub_arm_and_host_arm_succeeds();
    host_arm_recovers_a_stale_channel_arm();
    disconnected_slot_still_abandons();
    boot_default_target_is_first_ready_guest();
    central_backoff_is_bounded();
    input_pairing_allowed_with_unarmed_session();
    guest_pairing_refused_while_input_window_open();
    input_pairing_refused_and_ended_when_router_armed();
    begin_rpc_timeout_holds_guest_gate();
    forget_gated_by_the_host_route();
    puts("test_hub_worker: all passed");
    return 0;
}
