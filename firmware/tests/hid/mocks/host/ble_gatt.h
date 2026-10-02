/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Mirrors the NimBLE GATT declarations this HID service uses, enabling a
 * host-only compile and characteristic-registration contract test. */
#ifndef TEST_BLE_GATT_H
#define TEST_BLE_GATT_H
#include <stdint.h>
#include "os/os_mbuf.h"
#define BLE_UUID16_DECLARE(value) ((const void *)(uintptr_t)(value))
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_READ 0x0002
#define BLE_GATT_CHR_F_WRITE_NO_RSP 0x0004
#define BLE_GATT_CHR_F_WRITE 0x0008
#define BLE_GATT_CHR_F_NOTIFY 0x0010
#define BLE_GATT_CHR_F_READ_ENC 0x0200
#define BLE_GATT_CHR_F_WRITE_ENC 0x1000
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_GATT_ACCESS_OP_WRITE_DSC 3
struct ble_gatt_access_ctxt { uint8_t op; struct os_mbuf *om; uint16_t offset; };
typedef int ble_gatt_access_fn(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
struct ble_gatt_dsc_def {
    const void *uuid; uint8_t att_flags; uint8_t min_key_size;
    ble_gatt_access_fn *access_cb; void *arg;
};
struct ble_gatt_chr_def {
    const void *uuid; ble_gatt_access_fn *access_cb; void *arg;
    struct ble_gatt_dsc_def *descriptors; uint16_t flags;
    uint8_t min_key_size; uint16_t *val_handle;
};
struct ble_gatt_svc_def {
    uint8_t type; const void *uuid; const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *services);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *services);
int ble_gatts_notify_custom(uint16_t connection_handle, uint16_t value_handle, struct os_mbuf *packet);
#endif
