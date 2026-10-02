/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies that optional NimBLE guest startup stays disarmed, advertises one
 * composite identity, and closes failed or unexpected security transitions. */
#include <assert.h>
#include <string.h>

#include "hid_guest.h"
#include "hid_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

struct ble_hs_cfg ble_hs_cfg;
unsigned test_nvs_calls;
unsigned test_advertisements;
unsigned test_terminations;
int (*test_gap_callback)(struct ble_gap_event *, void *);
static hid_channel_t channel;

int nvs_flash_init(void) { test_nvs_calls++; return 0; }
int nimble_port_init(void) { return 0; }
int nimble_port_deinit(void) { return 0; }
void nimble_port_run(void) { }
void nimble_port_freertos_init(void (*task)(void *)) { (void)task; }
void nimble_port_freertos_deinit(void) { }
void ble_svc_gap_init(void) { }
void ble_svc_gatt_init(void) { }
int ble_svc_gap_device_name_set(const char *name) { return strcmp(name, "ESP32 KVM"); }
int ble_svc_gap_device_appearance_set(uint16_t appearance) { return appearance == 0x03c0 ? 0 : -1; }
void ble_store_config_init(void) { }
void ble_store_util_status_rr(void) { }
int ble_hs_util_ensure_addr(int privacy) { return privacy; }
int ble_hs_id_infer_auto(int privacy, uint8_t *type) { *type = 0; return privacy; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields)
{
    assert(fields->num_uuids16 == 1 && fields->uuids16[0].value == 0x1812);
    return 0;
}
int ble_gap_adv_start(uint8_t type, const void *address, uint32_t duration,
                      const struct ble_gap_adv_params *parameters,
                      int (*callback)(struct ble_gap_event *, void *), void *argument)
{
    (void)type; (void)address; (void)duration; (void)parameters; (void)argument;
    test_gap_callback = callback;
    test_advertisements++;
    return 0;
}
int ble_gap_terminate(uint16_t handle, uint8_t reason)
{ (void)handle; (void)reason; test_terminations++; return 0; }
int ble_gap_security_initiate(uint16_t handle) { return handle == 17 ? 0 : -1; }
int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *description)
{ description->conn_handle = handle; description->sec_state.encrypted = 1; description->sec_state.bonded = 1; return 0; }
int hid_gatt_register(void) { hid_channel_init(&channel, NULL, NULL); return 0; }
hid_channel_t *hid_gatt_channel(void) { return &channel; }
bool hid_gatt_on_connect(uint16_t handle)
{ if (channel.connected) return false; hid_channel_connected(&channel, handle); return true; }
void hid_gatt_on_disconnect(uint16_t handle)
{ if (channel.connected && channel.connection_handle == handle) hid_channel_disconnected(&channel); }
void hid_gatt_on_encryption(uint16_t handle, bool encrypted)
{ if (channel.connected && channel.connection_handle == handle) hid_channel_encrypted(&channel, encrypted); }
void hid_gatt_on_subscribe(uint16_t handle, uint16_t value_handle, bool enabled)
{ (void)handle; (void)value_handle; (void)enabled; }

extern unsigned test_nvs_calls;
extern unsigned test_advertisements;
extern unsigned test_terminations;
extern int (*test_gap_callback)(struct ble_gap_event *, void *);

int main(void)
{
    assert(hid_guest_start() == 0);
    assert(test_nvs_calls == 1);
    assert(!hid_gatt_channel()->armed);
    ble_hs_cfg.sync_cb();
    assert(test_advertisements == 1);

    struct ble_gap_event connect = {.type = BLE_GAP_EVENT_CONNECT};
    connect.connect.status = 0;
    connect.connect.conn_handle = 17;
    assert(test_gap_callback(&connect, NULL) == 0);
    assert(hid_gatt_channel()->connected && !hid_gatt_channel()->armed);
    assert(test_advertisements == 1);
    connect.connect.conn_handle = 18;
    assert(test_gap_callback(&connect, NULL) == 0);
    assert(test_terminations == 1);

    struct ble_gap_event encryption = {.type = BLE_GAP_EVENT_ENC_CHANGE};
    encryption.enc_change.conn_handle = 17;
    encryption.enc_change.status = 1;
    assert(test_gap_callback(&encryption, NULL) == 0);
    assert(test_terminations == 2 && !hid_gatt_channel()->armed);
    struct ble_gap_event disconnect = {.type = BLE_GAP_EVENT_DISCONNECT};
    disconnect.disconnect.conn.conn_handle = 17;
    assert(test_gap_callback(&disconnect, NULL) == 0);
    assert(test_advertisements == 2 && !hid_gatt_channel()->connected);
    connect.connect.conn_handle = 17;
    assert(test_gap_callback(&connect, NULL) == 0);
    encryption.enc_change.status = 0;
    assert(test_gap_callback(&encryption, NULL) == 0);
    assert(hid_gatt_channel()->encrypted && !hid_gatt_channel()->armed);
    return 0;
}
