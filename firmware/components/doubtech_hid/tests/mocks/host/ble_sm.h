/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Models NimBLE numeric comparison consent injection for host HID tests. */
#ifndef TEST_BLE_SM_H
#define TEST_BLE_SM_H
#include <stdint.h>
#define BLE_SM_IOACT_NUMCMP 4
struct ble_sm_io { uint8_t action; uint8_t numcmp_accept; };
int ble_sm_inject_io(uint16_t handle, struct ble_sm_io *io);
#endif
