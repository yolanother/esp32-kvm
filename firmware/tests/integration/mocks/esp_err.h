/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Portable stand-in for the ESP-IDF error type used by the HID headers. */
#ifndef ESP32_KVM_INTEGRATION_MOCK_ESP_ERR_H
#define ESP32_KVM_INTEGRATION_MOCK_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif
