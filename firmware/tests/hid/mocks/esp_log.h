/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Discards non-sensitive log messages during host-only lifecycle tests. */
#ifndef TEST_ESP_LOG_H
#define TEST_ESP_LOG_H
#define ESP_LOGE(tag, message) do { (void)(tag); (void)(message); } while (0)
#endif
