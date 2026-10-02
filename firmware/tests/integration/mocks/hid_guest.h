/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares a portable fake of the NimBLE-host request interface so the router
 * output adapter can be checked without ESP-IDF or radio hardware. */
#ifndef ESP32_KVM_INTEGRATION_MOCK_HID_GUEST_H
#define ESP32_KVM_INTEGRATION_MOCK_HID_GUEST_H
#include <stdbool.h>
#include <stdint.h>
#define HID_KEYBOARD_REPORT_LEN 8
typedef int esp_err_t;
bool hid_guest_request_ready(void);
bool hid_guest_request_arm(void);
bool hid_guest_request_release(void);
bool hid_guest_request_keyboard(const uint8_t keys[HID_KEYBOARD_REPORT_LEN]);
bool hid_guest_request_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                             int8_t wheel, int8_t pan);
bool hid_guest_request_consumer(uint16_t usage);
esp_err_t hid_guest_request_disconnect(void);
#endif
