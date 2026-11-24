/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares standard GATT service initialization for HID startup tests. */
#ifndef TEST_BLE_SVC_GATT_H
#define TEST_BLE_SVC_GATT_H
#include <stdint.h>
void ble_svc_gatt_init(void);
void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle);
#endif
