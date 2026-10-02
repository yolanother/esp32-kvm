/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the composite keyboard, relative mouse, and consumer HID report map
 * and one connection's fail-closed output gate. No Bluetooth stack is required
 * by this header, so report behavior can be tested before hardware flashing. */
#ifndef ESP32_KVM_HID_REPORT_H
#define ESP32_KVM_HID_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Input/output report IDs in the single composite HID service. */
enum hid_report_id {
    HID_REPORT_KEYBOARD = 1,
    HID_REPORT_MOUSE = 2,
    HID_REPORT_CONSUMER = 3
};

/** Report value sizes exclude the report ID, which is in Report Reference. */
enum {
    HID_KEYBOARD_REPORT_LEN = 8,
    HID_MOUSE_REPORT_LEN = 7,
    HID_CONSUMER_REPORT_LEN = 2
};

/** Composite HID report descriptor. */
extern const uint8_t hid_report_map[];
/** Number of bytes in hid_report_map. */
extern const size_t hid_report_map_len;

/** Sends one report to exactly one BLE connection; zero means enqueued. */
typedef int (*hid_send_fn)(void *context, uint16_t connection_handle,
                           uint8_t report_id, const uint8_t *bytes, size_t length);

/** Per-connection security, subscription, and held-report state. */
typedef struct {
    hid_send_fn send;
    void *send_context;
    uint16_t connection_handle;
    bool connected;
    bool encrypted;
    bool subscribed[4];
    bool armed;
    bool needs_disconnect;
    uint8_t protocol_mode;
    uint8_t keyboard_leds;
    uint8_t keyboard[HID_KEYBOARD_REPORT_LEN];
    uint8_t mouse_buttons;
    uint16_t consumer_usage;
} hid_channel_t;

/** Initializes an inert channel with a connection-addressed send callback. */
void hid_channel_init(hid_channel_t *channel, hid_send_fn send, void *context);
/** Records a new connection and clears all prior held state and subscriptions. */
void hid_channel_connected(hid_channel_t *channel, uint16_t connection_handle);
/** Drops a connection, disarms, and requires a new all-up baseline on reconnect. */
void hid_channel_disconnected(hid_channel_t *channel);
/** Updates encryption state; loss of encryption immediately disarms. */
void hid_channel_encrypted(hid_channel_t *channel, bool encrypted);
/** Updates one connection's report subscription; losing it disarms. */
void hid_channel_subscribed(hid_channel_t *channel, uint8_t report_id, bool subscribed);
/** Sends an all-up baseline and arms only when encrypted and fully subscribed. */
bool hid_channel_arm(hid_channel_t *channel);
/** Disarms first, then enqueues zero keyboard, mouse, and consumer reports. */
bool hid_channel_release(hid_channel_t *channel);
/** Enqueues a full eight-byte keyboard state when the channel is armed. */
bool hid_channel_keyboard(hid_channel_t *channel, const uint8_t keys[HID_KEYBOARD_REPORT_LEN]);
/** Enqueues signed relative motion and a five-button full state when armed. */
bool hid_channel_mouse(hid_channel_t *channel, uint8_t buttons, int16_t dx, int16_t dy,
                       int8_t wheel, int8_t pan);
/** Enqueues a consumer usage, where zero releases the held control. */
bool hid_channel_consumer(hid_channel_t *channel, uint16_t usage);
/** Accepts report protocol (1); boot protocol (0) is unsupported in v1. */
bool hid_channel_set_protocol_mode(hid_channel_t *channel, uint8_t mode);
/** Accepts the five defined keyboard LED bits from an encrypted guest. */
bool hid_channel_led_output(hid_channel_t *channel, uint8_t leds);

#endif
