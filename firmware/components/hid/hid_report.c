/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Provides the composite BLE HID report descriptor and per-connection report
 * gate. Report and Boot Protocol use distinct GATT input characteristics.
 * Transitions clear held input before arm; failed release or notification
 * disarms until the BLE link is recreated. */
#include "hid_report.h"

#include <string.h>

const uint8_t hid_report_map[] = {
    /* Keyboard: report ID 1, 8-byte input, 1-byte LED output. */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, HID_REPORT_KEYBOARD,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x75, 0x01, 0x95, 0x05, 0x91, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x91, 0x03,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0,

#ifndef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    /* Mouse: five buttons, signed 16-bit X/Y, signed 8-bit wheel/pan. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, HID_REPORT_MOUSE,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0x80, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x80, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x81, 0x06,
    0xc0, 0xc0,

    /* Consumer: one 16-bit usage; zero is release. */
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, HID_REPORT_CONSUMER,
    0x19, 0x00, 0x2a, 0xff, 0x03,
    0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0
#endif
};

const size_t hid_report_map_len = sizeof(hid_report_map);

static bool ready(const hid_channel_t *channel)
{
    if (!channel->connected || !channel->encrypted || channel->needs_disconnect) return false;
    if (channel->protocol_mode == 0)
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
        return channel->subscribed[HID_REPORT_BOOT_KEYBOARD];
#else
        return channel->subscribed[HID_REPORT_BOOT_KEYBOARD] &&
               channel->subscribed[HID_REPORT_BOOT_MOUSE];
#endif
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    return channel->subscribed[HID_REPORT_KEYBOARD];
#else
    return channel->subscribed[HID_REPORT_KEYBOARD] &&
           channel->subscribed[HID_REPORT_MOUSE] &&
           channel->subscribed[HID_REPORT_CONSUMER];
#endif
}

static bool send_report(hid_channel_t *channel, uint8_t id, const uint8_t *data, size_t size)
{
    if (channel->send(channel->send_context, channel->connection_handle, id, data, size) == 0)
        return true;
    channel->armed = false;
    return false;
}

static bool all_up(hid_channel_t *channel)
{
    static const uint8_t keyboard[HID_KEYBOARD_REPORT_LEN] = {0};
    static const uint8_t mouse[HID_MOUSE_REPORT_LEN] = {0};
    static const uint8_t consumer[HID_CONSUMER_REPORT_LEN] = {0};
    bool boot = channel->protocol_mode == 0;
    bool keyboard_ok = send_report(channel, boot ? HID_REPORT_BOOT_KEYBOARD : HID_REPORT_KEYBOARD,
                                   keyboard, sizeof(keyboard));
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    bool mouse_ok = true;
    bool consumer_ok = true;
#else
    bool mouse_ok = send_report(channel, boot ? HID_REPORT_BOOT_MOUSE : HID_REPORT_MOUSE,
                                mouse, boot ? 3 : sizeof(mouse));
    bool consumer_ok = boot || send_report(channel, HID_REPORT_CONSUMER, consumer,
                                           sizeof(consumer));
#endif
    memset(channel->keyboard, 0, sizeof(channel->keyboard));
    channel->mouse_buttons = 0;
    channel->consumer_usage = 0;
    return keyboard_ok && mouse_ok && consumer_ok;
}

void hid_channel_init(hid_channel_t *channel, hid_send_fn send, void *context)
{
    memset(channel, 0, sizeof(*channel));
    channel->send = send;
    channel->send_context = context;
    channel->protocol_mode = 1;
}

void hid_channel_connected(hid_channel_t *channel, uint16_t connection_handle)
{
    hid_send_fn send = channel->send;
    void *context = channel->send_context;
    hid_channel_init(channel, send, context);
    channel->connection_handle = connection_handle;
    channel->connected = true;
}

void hid_channel_disconnected(hid_channel_t *channel)
{
    hid_send_fn send = channel->send;
    void *context = channel->send_context;
    hid_channel_init(channel, send, context);
}

void hid_channel_encrypted(hid_channel_t *channel, bool encrypted)
{
    channel->encrypted = channel->connected && encrypted;
    if (!channel->encrypted) channel->armed = false;
}

void hid_channel_subscribed(hid_channel_t *channel, uint8_t report_id, bool subscribed)
{
    if (report_id < HID_REPORT_KEYBOARD || report_id > HID_REPORT_BOOT_MOUSE) return;
    channel->subscribed[report_id] = channel->connected && subscribed;
    if (!channel->subscribed[report_id]) channel->armed = false;
}

bool hid_channel_arm(hid_channel_t *channel)
{
    if (!ready(channel) || channel->armed) return false;
    if (!all_up(channel)) return false;
    channel->armed = true;
    return true;
}

bool hid_channel_release(hid_channel_t *channel)
{
    channel->armed = false;
    if (!ready(channel)) {
        channel->needs_disconnect = true;
        return false;
    }
    if (!all_up(channel)) {
        channel->needs_disconnect = true;
        return false;
    }
    return true;
}

bool hid_channel_keyboard(hid_channel_t *channel, const uint8_t keys[HID_KEYBOARD_REPORT_LEN])
{
    if (!channel->armed || !ready(channel) || keys[1] != 0) return false;
    if (!send_report(channel, channel->protocol_mode == 0 ? HID_REPORT_BOOT_KEYBOARD : HID_REPORT_KEYBOARD,
                     keys, HID_KEYBOARD_REPORT_LEN)) {
        channel->needs_disconnect = true;
        return false;
    }
    memcpy(channel->keyboard, keys, HID_KEYBOARD_REPORT_LEN);
    return true;
}

bool hid_channel_mouse(hid_channel_t *channel, uint8_t buttons, int16_t dx, int16_t dy,
                       int8_t wheel, int8_t pan)
{
    if (!channel->armed || !ready(channel) || (buttons & 0xe0u) != 0) return false;
    uint8_t report[HID_MOUSE_REPORT_LEN] = {
        buttons, (uint8_t)dx, (uint8_t)((uint16_t)dx >> 8),
        (uint8_t)dy, (uint8_t)((uint16_t)dy >> 8), (uint8_t)wheel, (uint8_t)pan
    };
    if (channel->protocol_mode == 0) {
        uint8_t boot[3] = {
            (uint8_t)(buttons & 0x07u),
            (uint8_t)(int8_t)(dx < -127 ? -127 : dx > 127 ? 127 : dx),
            (uint8_t)(int8_t)(dy < -127 ? -127 : dy > 127 ? 127 : dy)
        };
        if (!send_report(channel, HID_REPORT_BOOT_MOUSE, boot, sizeof(boot))) {
            channel->needs_disconnect = true;
            return false;
        }
        channel->mouse_buttons = buttons;
        return true;
    }
    if (!send_report(channel, HID_REPORT_MOUSE, report, sizeof(report))) {
        channel->needs_disconnect = true;
        return false;
    }
    channel->mouse_buttons = buttons;
    return true;
}

bool hid_channel_consumer(hid_channel_t *channel, uint16_t usage)
{
    if (!channel->armed || !ready(channel) || channel->protocol_mode == 0 ||
        usage > 0x03ffu) return false;
    uint8_t report[HID_CONSUMER_REPORT_LEN] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
    if (!send_report(channel, HID_REPORT_CONSUMER, report, sizeof(report))) {
        channel->needs_disconnect = true;
        return false;
    }
    channel->consumer_usage = usage;
    return true;
}

bool hid_channel_set_protocol_mode(hid_channel_t *channel, uint8_t mode)
{
    if (!channel->connected || !channel->encrypted || mode > 1) return false;
    if (mode == channel->protocol_mode) return true;
    if (channel->armed && !hid_channel_release(channel)) return false;
    channel->protocol_mode = mode;
    return true;
}

bool hid_channel_led_output(hid_channel_t *channel, uint8_t leds)
{
    if (!channel->connected || !channel->encrypted || (leds & 0xe0u) != 0) return false;
    channel->keyboard_leds = leds;
    return true;
}
