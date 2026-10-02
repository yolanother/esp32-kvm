/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares address and bond store helpers for NimBLE lifecycle tests. */
#ifndef TEST_BLE_UTIL_H
#define TEST_BLE_UTIL_H
int ble_hs_util_ensure_addr(int privacy);
void ble_store_util_status_rr(void);
#endif
