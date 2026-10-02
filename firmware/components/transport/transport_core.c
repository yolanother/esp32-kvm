/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Implements a bounded, binary-only HELLO/CAPS/SESSION_OPEN/GET_STATUS loopback
 * for ESP32-S3 USB CDC bring-up. Invalid or unsupported frames have no input
 * effects; the core never routes HID, logs bytes, or persists host data. */
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
    payload[at++] = 5; payload[at++] = 1; /* M0 loopback capacity, no READY slots */
    payload[at++] = 6; payload[at++] = 1; /* M0 loopback storage bound */
    payload[at++] = 7; payload[at++] = 0; /* no HID report features */
    payload[at++] = 8; at += cbor_uint(payload + at, core->session_id);
    emit(core, KVM_MSG_CAPS, seq, payload, at);
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
    }
    if (text_length < 1 || text_length > 32 || at + text_length + 2 > length) return false;
    at += text_length;
    if (p[at++] != 2) return false;
    /* The config revision is an unsigned CBOR integer; parse its bounded width. */
    if ((p[at] & 0xe0u) != 0) return false;
    unsigned info = p[at++] & 31u;
    size_t width = info < 24 ? 0 : info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 99;
    return at + width == length;
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
    const uint8_t *payload = p + KVM_PROTOCOL_HEADER_LEN;
    size_t payload_length = get_u16(p + 4);
    if (kind == KVM_MSG_HELLO) {
        if (session != 0 || payload_length != 6 || get_u16(payload) != 0 || get_u32(payload + 2) != 0) return;
        core->session_open = false;
        send_caps(core, seq);
        return;
    }
    if (session != core->session_id) return;
    if (kind == KVM_MSG_SESSION_OPEN && session_open_payload(payload, payload_length)) {
        uint8_t ack[10] = { KVM_MSG_SESSION_OPEN };
        put_u32(ack + 1, seq);
        core->session_open = true;
        emit(core, KVM_MSG_ACK, seq, ack, sizeof(ack));
    } else if (kind == KVM_MSG_GET_STATUS && core->session_open && payload_length == 0) {
        static const uint8_t status[] = { 0xa5, 1, 0, 2, 0, 3, 0x80, 4, 0, 5, 0 };
        emit(core, KVM_MSG_STATUS, seq, status, sizeof(status));
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
