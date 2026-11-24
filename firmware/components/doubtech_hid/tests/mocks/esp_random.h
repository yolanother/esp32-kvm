/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the deterministic random source used by host-side HID guest tests. */
#ifndef TEST_ESP_RANDOM_H
#define TEST_ESP_RANDOM_H
#include <stdint.h>
uint32_t esp_random(void);
#endif
