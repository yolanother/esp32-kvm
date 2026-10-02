/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the GAP service setup APIs used during HID startup. */
#ifndef TEST_BLE_SVC_GAP_H
#define TEST_BLE_SVC_GAP_H
#include <stdint.h>
void ble_svc_gap_init(void);
int ble_svc_gap_device_name_set(const char *name);
int ble_svc_gap_device_appearance_set(uint16_t appearance);
#endif
