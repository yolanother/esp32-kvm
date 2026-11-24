/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Models NimBLE's bond capacity, deletion, and store-overflow callbacks. */
#ifndef TEST_BLE_STORE_H
#define TEST_BLE_STORE_H
#include "host/ble_gap.h"
struct ble_store_status_event { int status; };
int ble_store_util_bonded_peers(ble_addr_t *peers, int *count, int maximum);
int ble_store_util_delete_peer(const ble_addr_t *peer);
#endif
