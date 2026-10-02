/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Models the small GAP event and advertising API surface used by single-guest
 * startup, so state transitions can be tested without a radio. */
#ifndef TEST_BLE_GAP_H
#define TEST_BLE_GAP_H
#include <stddef.h>
#include <stdint.h>
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_ENC_CHANGE 3
#define BLE_GAP_EVENT_SUBSCRIBE 4
#define BLE_GAP_EVENT_NOTIFY_TX 5
#define BLE_GAP_EVENT_ADV_COMPLETE 6
#define BLE_GAP_EVENT_REPEAT_PAIRING 7
#define BLE_GAP_EVENT_PASSKEY_ACTION 8
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HS_FOREVER 0xffffffffu
#define BLE_ERR_REM_USER_CONN_TERM 0x13
typedef struct { uint16_t value; } ble_uuid16_t;
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
#define BLE_UUID16_INIT(value) {value}
struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; };
struct ble_hs_adv_fields {
    uint8_t flags;
    uint8_t *name;
    size_t name_len;
    uint8_t name_is_complete;
    const ble_uuid16_t *uuids16;
    uint8_t num_uuids16;
    uint8_t uuids16_is_complete;
    uint16_t appearance;
    uint8_t appearance_is_present;
};
struct ble_gap_conn_desc {
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
    struct { uint8_t encrypted, bonded, authenticated; } sec_state;
};
struct ble_gap_event {
    int type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { struct ble_gap_conn_desc conn; } disconnect;
        struct { int status; uint16_t conn_handle; } enc_change;
        struct { uint16_t conn_handle, attr_handle; uint8_t cur_notify; } subscribe;
        struct { int status; uint16_t conn_handle; } notify_tx;
        struct { uint16_t conn_handle; } repeat_pairing;
        struct { uint16_t conn_handle; struct { uint8_t action; uint32_t numcmp; } params; } passkey;
    };
};
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields);
int ble_gap_adv_start(uint8_t own_address_type, const void *direct_address, uint32_t duration,
                      const struct ble_gap_adv_params *parameters,
                      int (*callback)(struct ble_gap_event *, void *), void *argument);
int ble_gap_terminate(uint16_t connection_handle, uint8_t reason);
int ble_gap_security_initiate(uint16_t connection_handle);
int ble_gap_conn_find(uint16_t connection_handle, struct ble_gap_conn_desc *description);
int ble_hs_id_infer_auto(int privacy, uint8_t *own_address_type);
#endif
