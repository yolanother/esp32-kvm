/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Minimal ESP result code for compiling opt-in BLE guest startup on the host. */
#ifndef TEST_ESP_ERR_H
#define TEST_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#endif
