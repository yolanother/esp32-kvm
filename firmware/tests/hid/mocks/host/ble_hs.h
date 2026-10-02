/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the NimBLE packet creation API used by connection-bound HID sends. */
#ifndef TEST_BLE_HS_H
#define TEST_BLE_HS_H
#include <stddef.h>
#include "os/os_mbuf.h"
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, size_t length);
#define BLE_SM_IO_CAP_NO_IO 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
struct ble_hs_cfg {
    void (*reset_cb)(int);
    void (*sync_cb)(void);
    void (*store_status_cb)(void);
    unsigned sm_io_cap, sm_bonding, sm_sc, sm_mitm;
    unsigned sm_our_key_dist, sm_their_key_dist;
};
extern struct ble_hs_cfg ble_hs_cfg;
#endif
