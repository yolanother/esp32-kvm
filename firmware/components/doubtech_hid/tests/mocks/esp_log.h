/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Discards non-sensitive log messages during host-only lifecycle tests. */
#ifndef TEST_ESP_LOG_H
#define TEST_ESP_LOG_H
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#endif
