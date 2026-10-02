/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Implements bounded binary USB framing, handshake and command dispatch for
 * ESP32-S3 CDC. Validated frames enter one serialized router; malformed or
 * stale frames have no HID effects and diagnostic text never enters CDC. */
#include "transport_core.h"
#include <string.h>

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32;
}
static void put_u16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put_u32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (i * 8));
}
static void put_u64(uint8_t *p, uint64_t n)
{
    put_u32(p, (uint32_t)n); put_u32(p + 4, (uint32_t)(n >> 32));
}

static uint32_t crc32c(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t cobs_decode(const uint8_t *encoded, size_t length, uint8_t *decoded)
{
    size_t src = 0, dst = 0;
    while (src < length) {
        uint8_t code = encoded[src++];
        if (!code || src + (size_t)code - 1 > length) return 0;
        for (unsigned i = 1; i < code; i++) {
            if (dst >= KVM_PROTOCOL_MAX_FRAME) return 0;
            decoded[dst++] = encoded[src++];
        }
        if (code != 0xff && src < length) {
            if (dst >= KVM_PROTOCOL_MAX_FRAME) return 0;
            decoded[dst++] = 0;
        }
    }
    return dst;
}

static size_t cobs_encode(const uint8_t *decoded, size_t length, uint8_t *encoded)
{
    size_t code_at = 0, dst = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < length; i++) {
        if (decoded[i] == 0) {
            encoded[code_at] = code;
            code_at = dst++;
            code = 1;
        } else {
            encoded[dst++] = decoded[i];
            if (++code == 0xff) {
                encoded[code_at] = code;
                code_at = dst++;
                code = 1;
            }
        }
    }
    encoded[code_at] = code;
    encoded[dst++] = 0;
    return dst;
}

static size_t cbor_uint(uint8_t *out, uint64_t n)
{
    if (n < 24) { out[0] = (uint8_t)n; return 1; }
    if (n <= UINT8_MAX) { out[0] = 0x18; out[1] = (uint8_t)n; return 2; }
    if (n <= UINT16_MAX) { out[0] = 0x19; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n; return 3; }
    if (n <= UINT32_MAX) {
        out[0] = 0x1a;
        for (unsigned i = 0; i < 4; i++) out[1 + i] = (uint8_t)(n >> (24 - i * 8));
        return 5;
    }
    out[0] = 0x1b;
    for (unsigned i = 0; i < 8; i++) out[1 + i] = (uint8_t)(n >> (56 - i * 8));
    return 9;
}

static size_t cbor_text(uint8_t *out, const char *text)
{
    size_t length = strlen(text);
    size_t header = length < 24 ? 1 : 2;
    out[0] = length < 24 ? (uint8_t)(0x60u + length) : 0x78u;
    if (header == 2) out[1] = (uint8_t)length;
    memcpy(out + header, text, length);
    return header + length;
}

static void emit(kvm_transport_core_t *core, uint8_t kind, uint32_t seq,
                 const uint8_t *payload, size_t payload_length)
{
    uint8_t frame[KVM_PROTOCOL_MAX_FRAME];
    uint8_t encoded[KVM_TRANSPORT_ENCODED_CAPACITY];
    size_t length = KVM_PROTOCOL_HEADER_LEN + payload_length + 4;
    if (payload_length > KVM_PROTOCOL_MAX_PAYLOAD) return;
    memset(frame, 0, KVM_PROTOCOL_HEADER_LEN);
    put_u16(frame, KVM_PROTOCOL_MAGIC);
    frame[2] = KVM_PROTOCOL_MAJOR;
    frame[3] = kind;
    put_u16(frame + 4, (uint16_t)payload_length);
    put_u64(frame + 8, core->session_id);
    put_u32(frame + 16, seq);
    put_u32(frame + 20, core->router ? core->router->generation : 0);
    if (payload_length) memcpy(frame + KVM_PROTOCOL_HEADER_LEN, payload, payload_length);
    put_u32(frame + length - 4, crc32c(frame, length - 4));
    core->send(core->send_context, encoded, cobs_encode(frame, length, encoded));
}

static void send_caps(kvm_transport_core_t *core, uint32_t seq)
{
    uint8_t payload[96];
    size_t at = 0;
    payload[at++] = 0xa8;
    payload[at++] = 1; at += cbor_text(payload + at, core->firmware_version);
    payload[at++] = 2; at += cbor_text(payload + at, core->board_id);
    payload[at++] = 3; payload[at++] = 0; /* minor minimum */
    payload[at++] = 4; payload[at++] = 0; /* minor maximum */
    payload[at++] = 5; payload[at++] = 1; /* One live BLE guest. */
    payload[at++] = 6; payload[at++] = 1; /* Conservative storage bound. */
    payload[at++] = 7; payload[at++] = 0; /* Report feature flags not negotiated. */
    payload[at++] = 8; at += cbor_uint(payload + at, core->session_id);
    emit(core, KVM_MSG_CAPS, seq, payload, at);
}

static uint64_t firmware_ms(kvm_transport_core_t *core)
{
    return core->now ? core->now(core->now_context) : 0;
}

static void send_result(kvm_transport_core_t *core, uint8_t original,
                        uint32_t seq, kvm_router_result_t result)
{
    uint8_t payload[10] = {original};
    put_u32(payload + 1, seq);
    payload[5] = (uint8_t)result;
    put_u32(payload + 6, core->router ? core->router->last_result_generation : 0);
    emit(core, result == KVM_ROUTER_OK ? KVM_MSG_ACK : KVM_MSG_NACK,
         seq, payload, sizeof(payload));
}

static void send_status(kvm_transport_core_t *core, uint32_t seq)
{
    uint8_t p[48] = {0xa5, 1, 0, 2, 0, 3, 0x81, 0xa5, 1, 1, 2, 0x50};
    kvm_router_t *r = core->router;
    bool is_ready = r && r->output.ready && r->output.ready(r->context, 1);
    /* Slot map contains a zero opaque token until bonding identity is wired. */
    size_t at = 28;
    p[at++] = 3; p[at++] = is_ready ? 0xf5 : 0xf4;
    p[at++] = 4; p[at++] = is_ready ? 0xf5 : 0xf4;
    p[at++] = 5; p[at++] = 0;
    p[at++] = 4; p[at++] = r && r->fault ? 1 : 0;
    p[at++] = 5; at += cbor_uint(p + at, r ? r->generation : 0);
    p[2] = r && r->armed ? 2 : r && r->slot ? 1 : 0;
    p[4] = r ? r->slot : 0;
    emit(core, KVM_MSG_STATUS, seq, p, at);
    core->last_status_ms = firmware_ms(core);
    core->status_generation = r ? r->generation : 0;
    core->status_slot = r ? r->slot : 0;
    core->status_armed = r && r->armed;
    core->status_fault = r && r->fault;
}

static bool utf8_valid(const uint8_t *p, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint8_t first = p[i++];
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned more = first >= 0xc2 && first <= 0xdf ? 1 :
                        first >= 0xe0 && first <= 0xef ? 2 :
                        first >= 0xf0 && first <= 0xf4 ? 3 : 99;
        if (more == 99 || i + more > length) return false;
        uint8_t second = p[i];
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        for (unsigned j = 0; j < more; j++) if ((p[i++] & 0xc0u) != 0x80u) return false;
    }
    return true;
}

static bool session_open_payload(const uint8_t *p, size_t length)
{
    size_t at = 0, text_length;
    if (length < 6 || p[at++] != 0xa2 || p[at++] != 1) return false;
    if ((p[at] & 0xe0u) != 0x60u) return false;
    text_length = p[at] & 31u;
    at++;
    if (text_length == 24) {
        if (at >= length) return false;
        text_length = p[at++];
        if (text_length < 24) return false;
    }
    if (text_length < 1 || text_length > 32 || at + text_length + 2 > length ||
        !utf8_valid(p + at, text_length)) return false;
    at += text_length;
    if (p[at++] != 2) return false;
    /* The config revision is an unsigned CBOR integer; parse its bounded width. */
    if ((p[at] & 0xe0u) != 0) return false;
    unsigned info = p[at++] & 31u;
    size_t width = info < 24 ? 0 : info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 99;
    if (at + width != length) return false;
    uint64_t number = info < 24 ? info : 0;
    for (size_t i = 0; i < width; i++) number = (number << 8) | p[at + i];
    if ((width == 1 && number < 24) || (width == 2 && number <= UINT8_MAX) ||
        (width == 4 && number <= UINT16_MAX) || (width == 8 && number <= UINT32_MAX))
        return false;
    return true;
}

static void handle_frame(kvm_transport_core_t *core, const uint8_t *p, size_t length)
{
    if (length < KVM_PROTOCOL_HEADER_LEN + 4 || length > KVM_PROTOCOL_MAX_FRAME ||
        get_u16(p) != KVM_PROTOCOL_MAGIC || p[2] != KVM_PROTOCOL_MAJOR ||
        get_u16(p + 6) != 0 || get_u16(p + 4) > KVM_PROTOCOL_MAX_PAYLOAD ||
        length != KVM_PROTOCOL_HEADER_LEN + get_u16(p + 4) + 4 ||
        get_u32(p + length - 4) != crc32c(p, length - 4)) return;

    uint8_t kind = p[3];
    uint64_t session = get_u64(p + 8);
    uint32_t seq = get_u32(p + 16);
    uint32_t generation = get_u32(p + 20);
    const uint8_t *payload = p + KVM_PROTOCOL_HEADER_LEN;
    size_t payload_length = get_u16(p + 4);
    if (kind == KVM_MSG_HELLO) {
        if (session != 0 || generation != 0 || payload_length != 6 ||
            get_u16(payload) != 0 || get_u32(payload + 2) != 0) return;
        core->session_open = false;
        if (core->router) kvm_router_reset(core->router);
        send_caps(core, seq);
        return;
    }
    if (session != core->session_id) return;
    if (kind == KVM_MSG_SESSION_OPEN && session_open_payload(payload, payload_length)) {
        uint8_t ack[10] = { KVM_MSG_SESSION_OPEN };
        put_u32(ack + 1, seq);
        if (!core->session_open && core->router)
            kvm_router_session_open(core->router, core->session_id, firmware_ms(core));
        core->session_open = true;
        put_u32(ack + 6, core->router ? core->router->generation : 0);
        emit(core, KVM_MSG_ACK, seq, ack, sizeof(ack));
    } else if (kind == KVM_MSG_GET_STATUS && core->session_open && payload_length == 0) {
        send_status(core, seq);
    } else if (core->session_open && core->router) {
        kvm_router_t *r = core->router;
        kvm_router_result_t result = KVM_ROUTER_BAD_PAYLOAD;
        uint64_t now_ms = firmware_ms(core);
        if (kind == KVM_MSG_HEARTBEAT) {
            if (payload_length == 8) result = kvm_router_heartbeat(r, session, get_u64(payload), now_ms);
            if (result != KVM_ROUTER_OK) {
                r->last_result_generation = r->generation;
                send_result(core, kind, seq, result);
            }
        } else if (kind == KVM_MSG_SWITCH) {
            if (payload_length == 9 && generation == get_u32(payload + 1))
                result = kvm_router_switch(r, session, seq, payload[0], get_u32(payload + 1),
                                           get_u32(payload + 5), now_ms);
            else r->last_result_generation = r->generation;
            send_result(core, kind, seq, result);
        } else if (kind == KVM_MSG_RELEASE_ALL) {
            if (!payload_length) result = kvm_router_release_all(r, session, seq, now_ms);
            else r->last_result_generation = r->generation;
            send_result(core, kind, seq, result);
        } else if (kind == KVM_MSG_ARM) {
            if (payload_length == 5 && generation == get_u32(payload + 1))
                result = kvm_router_arm(r, session, seq, payload[0], get_u32(payload + 1), now_ms);
            else r->last_result_generation = r->generation;
            send_result(core, kind, seq, result);
        } else if (kind == KVM_MSG_KEY_STATE && payload_length == 8) {
            kvm_router_input_t input = {0};
            input.kind = KVM_ROUTER_KEYBOARD;
            memcpy(input.keyboard, payload, 8);
            (void)kvm_router_input(r, session, generation, seq, input, now_ms);
        } else if (kind == KVM_MSG_POINTER && payload_length == 7) {
            kvm_router_input_t input = {0};
            input.kind = KVM_ROUTER_POINTER;
            input.buttons = payload[0];
            input.dx = (int16_t)get_u16(payload + 1);
            input.dy = (int16_t)get_u16(payload + 3);
            input.wheel = (int8_t)payload[5];
            input.pan = (int8_t)payload[6];
            (void)kvm_router_input(r, session, generation, seq, input, now_ms);
        } else if (kind == KVM_MSG_CONSUMER_STATE && payload_length == 2) {
            kvm_router_input_t input = {0};
            input.kind = KVM_ROUTER_CONSUMER;
            input.consumer = get_u16(payload);
            (void)kvm_router_input(r, session, generation, seq, input, now_ms);
        }
    }
}

bool kvm_transport_core_init(kvm_transport_core_t *core, const char *board_id,
                             const char *firmware_version, uint64_t session_id,
                             kvm_transport_send_fn send, void *send_context)
{
    if (!core || !board_id || !firmware_version || !send || !session_id) return false;
    size_t board_length = strnlen(board_id, 33);
    size_t version_length = strnlen(firmware_version, 33);
    if (!board_length || board_length > 32 || !version_length || version_length > 32) return false;
    memset(core, 0, sizeof(*core));
    memcpy(core->board_id, board_id, board_length);
    memcpy(core->firmware_version, firmware_version, version_length);
    core->session_id = session_id;
    core->send = send;
    core->send_context = send_context;
    return true;
}

void kvm_transport_core_reset(kvm_transport_core_t *core)
{
    if (!core) return;
    core->rx_length = 0;
    core->draining = false;
    core->session_open = false;
    if (core->router) kvm_router_reset(core->router);
}

void kvm_transport_core_feed(kvm_transport_core_t *core, const uint8_t *bytes, size_t length)
{
    if (!core || (!bytes && length)) return;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == 0) {
            if (!core->draining && core->rx_length) {
                uint8_t decoded[KVM_PROTOCOL_MAX_FRAME];
                size_t decoded_length = cobs_decode(core->rx, core->rx_length, decoded);
                if (decoded_length) handle_frame(core, decoded, decoded_length);
            }
            core->rx_length = 0;
            core->draining = false;
        } else if (!core->draining) {
            if (core->rx_length >= KVM_TRANSPORT_ENCODED_CAPACITY - 1) {
                core->rx_length = 0;
                core->draining = true;
            } else core->rx[core->rx_length++] = bytes[i];
        }
    }
}

bool kvm_transport_core_session_open(const kvm_transport_core_t *core)
{
    return core && core->session_open;
}

void kvm_transport_core_bind_router(kvm_transport_core_t *core, kvm_router_t *router,
                                    kvm_transport_now_fn now, void *now_context)
{
    if (!core) return;
    core->router = router;
    core->now = now;
    core->now_context = now_context;
}

void kvm_transport_core_tick(kvm_transport_core_t *core)
{
    if (!core || !core->router || !core->now) return;
    kvm_router_tick(core->router, firmware_ms(core));
    if (!core->session_open) return;
    kvm_router_t *r = core->router;
    uint64_t now_ms = firmware_ms(core);
    if (now_ms < core->last_status_ms || now_ms - core->last_status_ms >= 1000 ||
        r->generation != core->status_generation || r->slot != core->status_slot ||
        r->armed != core->status_armed || r->fault != core->status_fault)
        send_status(core, 0);
}
