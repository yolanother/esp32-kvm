/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares NVS initialization for host-only BLE lifecycle tests. */
#ifndef TEST_NVS_FLASH_H
#define TEST_NVS_FLASH_H
#include "esp_err.h"
esp_err_t nvs_flash_init(void);
#endif
