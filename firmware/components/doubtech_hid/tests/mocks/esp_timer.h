/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares monotonic microseconds for host-side pairing expiry tests. */
#ifndef TEST_ESP_TIMER_H
#define TEST_ESP_TIMER_H
#include <stdint.h>
int64_t esp_timer_get_time(void);
#endif
