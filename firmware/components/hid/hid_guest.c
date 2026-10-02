/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Starts an optional single-identity NimBLE HID peripheral after explicit
 * firmware integration. It advertises a composite HID service, requires an
 * encrypted bonded link, and leaves routing disarmed until the host actor arms. */
#include "hid_guest.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "hid_gatt.h"

/* ESP-IDF's NimBLE store configuration helper has no public header. */
void ble_store_config_init(void);

static const char *tag = "esp32-kvm-hid";
static uint8_t own_address_type;

static int gap_event(struct ble_gap_event *event, void *argument);

static void advertise(void)
{
    static const ble_uuid16_t service[] = {BLE_UUID16_INIT(0x1812)};
    static const char name[] = "ESP32 KVM";
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params parameters = {0};
    if (hid_gatt_channel()->connected) return;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = sizeof(name) - 1;
    fields.name_is_complete = 1;
    fields.uuids16 = service;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = 0x03c0; /* Generic HID appearance. */
    fields.appearance_is_present = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(tag, "HID advertising fields rejected");
        return;
    }
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER,
                          &parameters, gap_event, NULL) != 0)
        ESP_LOGE(tag, "HID advertising failed");
}

static void on_reset(int reason)
{
    (void)reason;
    hid_channel_disconnected(hid_gatt_channel());
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &own_address_type) != 0) {
        ESP_LOGE(tag, "No stable BLE identity available");
        return;
    }
    advertise();
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    hid_channel_t *channel = hid_gatt_channel();
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) { advertise(); return 0; }
        if (!hid_gatt_on_connect(event->connect.conn_handle)) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (ble_gap_security_initiate(event->connect.conn_handle) != 0)
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        hid_gatt_on_disconnect(event->disconnect.conn.conn_handle);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc description;
        bool secure = event->enc_change.status == 0 &&
                      ble_gap_conn_find(event->enc_change.conn_handle, &description) == 0 &&
                      description.sec_state.encrypted && description.sec_state.bonded;
        hid_gatt_on_encryption(event->enc_change.conn_handle, secure);
        if (!secure) ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        hid_gatt_on_subscribe(event->subscribe.conn_handle,
                              event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status != 0 && channel->connected &&
            channel->connection_handle == event->notify_tx.conn_handle) {
            channel->armed = false;
            channel->needs_disconnect = true;
            ble_gap_terminate(channel->connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Preserve the old bond; deletion requires explicit user intent. */
        ble_gap_terminate(event->repeat_pairing.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    default: return 0;
    }
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t hid_guest_start(void)
{
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result; /* Never erase stored bonds implicitly. */
    result = nimble_port_init();
    if (result != ESP_OK) return result;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0; /* Bring-up pairing is Just Works, pending UI approval. */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = hid_gatt_register();
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    result = ble_svc_gap_device_name_set("ESP32 KVM");
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    result = ble_svc_gap_device_appearance_set(0x03c0);
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
