/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Sends independently encoded USB frames through transport and router with a
 * fake one-guest HID sink, checking command dispatch, ACKs, report fencing,
 * malformed input rejection, lease expiry, and disconnect release. */
#include "transport_core.h"
#include "router.h"
#include <assert.h>
#include <string.h>

static uint8_t reply[1024];
static size_t reply_len;
static int replies, releases, arms, sends;
static uint64_t now_ms;
static bool ready = true;

static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t v = UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        v ^= p[i];
        for (int b = 0; b < 8; b++) v = (v >> 1) ^ (0x82f63b78u & (0u - (v & 1u)));
    }
    return ~v;
}
static void u32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i));
}
static void u64(uint8_t *p, uint64_t n)
{
    u32(p, (uint32_t)n); u32(p + 4, (uint32_t)(n >> 32));
}
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
{
    (void)context; assert(n <= sizeof(reply)); memcpy(reply, p, n); reply_len = n; replies++;
}
static uint64_t clock_ms(void *context) { (void)context; return now_ms; }
static bool output_ready(void *context, uint8_t slot) { (void)context; return slot == 1 && ready; }
static bool output_release(void *context, uint8_t slot) { (void)context; assert(slot == 1); releases++; return true; }
static bool output_arm(void *context, uint8_t slot) { (void)context; assert(slot == 1); arms++; return true; }
static bool output_send(void *context, uint8_t slot, const kvm_router_input_t *input)
{ (void)context; assert(slot == 1 && input->kind == KVM_ROUTER_KEYBOARD); sends++; return true; }
static void output_disconnect(void *context, uint8_t slot) { (void)context; assert(slot == 1); }
static void send_frame(kvm_transport_core_t *core, uint8_t kind, uint64_t session,
                       uint32_t seq, uint32_t generation, const uint8_t *payload, size_t n)
{
    uint8_t encoded[600]; size_t len = frame(encoded, kind, session, seq, generation, payload, n);
    kvm_transport_core_feed(core, encoded, len);
}
static void expect_reply(uint8_t kind, uint8_t error, uint32_t generation)
{
    uint8_t decoded[600]; size_t n = decode(decoded);
    assert(n >= 28 && decoded[3] == kind);
    if (kind == KVM_MSG_ACK || kind == KVM_MSG_NACK) {
        assert(decoded[29] == error);
        assert(((uint32_t)decoded[30] | ((uint32_t)decoded[31] << 8) |
                ((uint32_t)decoded[32] << 16) | ((uint32_t)decoded[33] << 24)) == generation);
    }
}
int main(void)
{
    kvm_router_t router;
    kvm_router_output_t output = {output_ready, output_release, output_arm, output_send, output_disconnect};
    kvm_transport_core_t core;
    const uint8_t hello[] = {0, 0, 0, 0, 0, 0};
    const uint8_t open[] = {0xa2, 1, 0x61, 'h', 2, 0};
    uint8_t command[9], key[8] = {0};
    kvm_router_init(&router, output, NULL);
    assert(kvm_transport_core_init(&core, "b", "1", 7, capture, NULL));
    kvm_transport_core_bind_router(&core, &router, clock_ms, NULL);
    send_frame(&core, KVM_MSG_HELLO, 0, 1, 0, hello, sizeof(hello));
    expect_reply(KVM_MSG_CAPS, 0, 0);
    send_frame(&core, KVM_MSG_SESSION_OPEN, 7, 2, 0, open, sizeof(open));
    expect_reply(KVM_MSG_ACK, 0, 0);
    send_frame(&core, KVM_MSG_GET_STATUS, 7, 21, 0, NULL, 0);
    expect_reply(KVM_MSG_STATUS, 0, 0);
    command[0] = 1; u32(command + 1, 0); u32(command + 5, 1);
    send_frame(&core, KVM_MSG_SWITCH, 7, 3, 0, command, 9);
    expect_reply(KVM_MSG_ACK, 0, 1);
    send_frame(&core, KVM_MSG_SWITCH, 7, 3, 0, command, 9);
    assert(router.generation == 1 && releases == 0);
    command[0] = 1; u32(command + 1, 1);
    send_frame(&core, KVM_MSG_ARM, 7, 4, 1, command, 5);
    expect_reply(KVM_MSG_ACK, 0, 1);
    assert(arms == 1 && router.armed);
    key[2] = 4;
    send_frame(&core, KVM_MSG_KEY_STATE, 7, 5, 0, key, 8);
    assert(router.queued == 0 && sends == 0);
    send_frame(&core, KVM_MSG_KEY_STATE, 7, 6, 1, key, 8);
    kvm_transport_core_tick(&core);
    assert(sends == 1);
    now_ms = 500;
    kvm_transport_core_tick(&core);
    assert(!router.armed && router.slot == 0 && releases == 1);
    send_frame(&core, KVM_MSG_KEY_STATE, 7, 7, 1, key, 8);
    assert(sends == 1);
    kvm_transport_core_reset(&core);
    assert(!router.session_open);
    uint8_t invalid_open[] = {0xa2, 1, 0x61, 0, 2, 0};
    int before = replies;
    send_frame(&core, KVM_MSG_SESSION_OPEN, 7, 8, 0, invalid_open, sizeof(invalid_open));
    assert(replies == before && !router.session_open);
    return 0;
}
