/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Models the small NVS blob API used for persistent HID bond token metadata. */
#ifndef TEST_NVS_H
#define TEST_NVS_H
#include <stddef.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
int nvs_open(const char *namespace_name, int mode, nvs_handle_t *handle);
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
int nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
