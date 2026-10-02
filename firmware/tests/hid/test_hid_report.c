/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the composite HID descriptor's actual bit widths and the disarmed,
 * encrypted, connection-addressed report state machine without an ESP-IDF SDK. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hid_report.h"

typedef struct {
    uint16_t handle;
    uint8_t report_id;
    size_t length;
    uint8_t bytes[HID_MOUSE_REPORT_LEN];
    unsigned calls;
    unsigned fail_at;
} sink_t;

static int capture(void *context, uint16_t handle, uint8_t report_id,
                   const uint8_t *bytes, size_t length)
{
    sink_t *sink = context;
    sink->calls++;
    if (sink->fail_at == sink->calls) return -1;
    sink->handle = handle;
    sink->report_id = report_id;
    sink->length = length;
    memcpy(sink->bytes, bytes, length);
    return 0;
}

static void test_descriptor(void)
{
    size_t bits_in[4] = {0}, bits_out[4] = {0};
    unsigned applications = 0;
    unsigned open = 0;
    unsigned id = 0, width = 0, count = 0;
    for (size_t at = 0; at < hid_report_map_len;) {
        uint8_t prefix = hid_report_map[at++];
        assert(prefix != 0xfe); /* no long items */
        unsigned size = prefix & 3u;
        if (size == 3) size = 4;
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        assert(at + size <= hid_report_map_len);
        unsigned value = 0;
        for (unsigned i = 0; i < size; i++) value |= (unsigned)hid_report_map[at + i] << (8 * i);
        at += size;
        if (type == 1 && tag == 8) { id = value; assert(id >= 1 && id <= 3); }
        if (type == 1 && tag == 7) width = value;
        if (type == 1 && tag == 9) count = value;
        if (type == 0 && tag == 10) { open++; if (value == 1) applications++; }
        if (type == 0 && tag == 12) { assert(open); open--; }
        if (type == 0 && tag == 8) bits_in[id] += width * count;
        if (type == 0 && tag == 9) bits_out[id] += width * count;
    }
    assert(applications == 3 && open == 0);
    assert(bits_in[HID_REPORT_KEYBOARD] == 64);
    assert(bits_in[HID_REPORT_MOUSE] == 56);
    assert(bits_in[HID_REPORT_CONSUMER] == 16);
    assert(bits_out[HID_REPORT_KEYBOARD] == 8);
}

static void ready(hid_channel_t *channel)
{
    hid_channel_connected(channel, 17);
    hid_channel_encrypted(channel, true);
    hid_channel_subscribed(channel, HID_REPORT_KEYBOARD, true);
    hid_channel_subscribed(channel, HID_REPORT_MOUSE, true);
    hid_channel_subscribed(channel, HID_REPORT_CONSUMER, true);
}

static void test_all_up_and_gating(void)
{
    hid_channel_t channel;
    sink_t sink = {0};
    uint8_t keys[HID_KEYBOARD_REPORT_LEN] = {2, 0, 4};
    hid_channel_init(&channel, capture, &sink);
    assert(!hid_channel_keyboard(&channel, keys));
    hid_channel_connected(&channel, 17);
    assert(!hid_channel_arm(&channel));
    hid_channel_encrypted(&channel, true);
    assert(!hid_channel_arm(&channel));
    hid_channel_subscribed(&channel, HID_REPORT_KEYBOARD, true);
    hid_channel_subscribed(&channel, HID_REPORT_MOUSE, true);
    hid_channel_subscribed(&channel, HID_REPORT_CONSUMER, true);
    assert(hid_channel_arm(&channel));
    assert(sink.calls == 3 && sink.report_id == HID_REPORT_CONSUMER);
    assert(hid_channel_keyboard(&channel, keys));
    assert(sink.handle == 17 && sink.report_id == HID_REPORT_KEYBOARD);
    assert(sink.length == HID_KEYBOARD_REPORT_LEN && sink.bytes[2] == 4);
    assert(hid_channel_mouse(&channel, 1, -32768, 32767, -1, 1));
    assert(sink.length == HID_MOUSE_REPORT_LEN);
    assert(sink.bytes[0] == 1 && sink.bytes[1] == 0 && sink.bytes[2] == 0x80);
    assert(sink.bytes[3] == 0xff && sink.bytes[4] == 0x7f);
    assert(hid_channel_consumer(&channel, 0x00e9));
    assert(hid_channel_release(&channel));
    assert(!channel.armed && !hid_channel_keyboard(&channel, keys));
    assert(sink.report_id == HID_REPORT_CONSUMER && sink.bytes[0] == 0);
    assert(hid_channel_arm(&channel));
}

static void test_disconnect_failure_and_protocol_mode(void)
{
    hid_channel_t channel;
    sink_t sink = {0};
    hid_channel_init(&channel, capture, &sink);
    ready(&channel);
    sink.fail_at = 2;
    assert(!hid_channel_arm(&channel));
    assert(!channel.armed);
    sink.fail_at = 0;
    assert(hid_channel_arm(&channel));
    assert(!hid_channel_set_protocol_mode(&channel, 0)); /* boot unsupported */
    assert(hid_channel_set_protocol_mode(&channel, 1));
    assert(hid_channel_led_output(&channel, 0x1f));
    assert(channel.keyboard_leds == 0x1f);
    assert(!hid_channel_led_output(&channel, 0x80));
    assert(hid_channel_mouse(&channel, 1, 0, 0, 0, 0));
    hid_channel_disconnected(&channel);
    assert(!channel.armed && channel.mouse_buttons == 0);
    assert(!hid_channel_mouse(&channel, 0, 0, 0, 0, 0));
    ready(&channel);
    assert(hid_channel_arm(&channel)); /* fresh all-up baseline */
    sink.fail_at = sink.calls + 1;
    assert(!hid_channel_mouse(&channel, 1, 0, 0, 0, 0));
    assert(channel.needs_disconnect && !channel.armed);
    sink.fail_at = 0;
    assert(!hid_channel_arm(&channel));
    hid_channel_disconnected(&channel);
    ready(&channel);
    assert(hid_channel_arm(&channel));
    assert(hid_channel_keyboard(&channel, (uint8_t[8]){0,0,4}));
    sink.fail_at = sink.calls + 1;
    assert(!hid_channel_release(&channel));
    assert(channel.needs_disconnect && !channel.armed);
}

int main(void)
{
    test_descriptor();
    test_all_up_and_gating();
    test_disconnect_failure_and_protocol_mode();
    return 0;
}
