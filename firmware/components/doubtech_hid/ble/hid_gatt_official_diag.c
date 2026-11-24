/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Diagnostic adapter around ESP-IDF's supported NimBLE HID service builder.
 * It preserves the KVM channel API while removing our custom GATT database
 * from the macOS compatibility test. */
#include "hid_gatt.h"

#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "services/bas/ble_svc_bas.h"
#include "services/hid/ble_svc_hid.h"
#include "host/ble_att.h"
#include "esp_log.h"

static hid_channel_t channels[DOUBTECH_HID_MAX_CONNECTIONS];
static uint16_t keyboard_handle;
static uint16_t led_handle;
static uint16_t boot_keyboard_handle;
static unsigned report_characteristic_index;

/* The service builder owns mutable definitions for the lifetime of the server.
 * Preserve its callbacks and arguments while instrumenting HID accesses only. */
static struct {
    uint16_t handle, uuid;
    ble_gatt_access_fn *callback;
    void *argument;
} traced[24];
static size_t traced_count;

static int read_pnp_id(uint16_t connection, uint16_t handle,
                        struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)connection; (void)handle; (void)argument;
    /* The SDK DIS callback prepends a Bluetooth vendor flag and uses strlen,
     * which cannot represent a binary USB PnP ID containing zero bytes. */
    static const uint8_t pnp_id[] = {0x02, 0x3a, 0x30, 0x01, 0x10, 0x00, 0x01};
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    int result = os_mbuf_append(context->om, pnp_id, sizeof(pnp_id));
    ESP_LOGI("kvm-hid-trace", "PnP ID read conn=%u handle=%u len=%u result=%d",
             connection, handle, (unsigned)sizeof(pnp_id), result);
    return result == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int read_device_info(uint16_t connection, uint16_t handle,
                             struct ble_gatt_access_ctxt *context, void *argument)
{
    uintptr_t uuid = (uintptr_t)argument;
    if (uuid == 0x2a50) return read_pnp_id(connection, handle, context, NULL);
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    const char *value = uuid == 0x2a24 ? "ESP32 KVM S3" : "ESP32 KVM";
    return os_mbuf_append(context->om, value, strlen(value)) == 0 ?
           0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* The SDK defaults omit the mandatory PnP ID. Register a complete DIS with
 * binary-safe reads rather than its text-only placeholder characteristics. */
static const struct ble_gatt_chr_def device_info_characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2a24), .access_cb = read_device_info,
     .arg = (void *)0x2a24, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(0x2a29), .access_cb = read_device_info,
     .arg = (void *)0x2a29, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(0x2a50), .access_cb = read_device_info,
     .arg = (void *)0x2a50, .flags = BLE_GATT_CHR_F_READ},
    {0}
};
static const struct ble_gatt_svc_def device_info_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180a),
     .characteristics = device_info_characteristics},
    {0}
};

static void trace_bytes(const char *operation, uint16_t handle,
                         const uint8_t *bytes, size_t length)
{
    (void)operation; (void)handle;
    static const char digits[] = "0123456789abcdef";
    char hex[193];
    if (length > 96) length = 96;
    for (size_t i = 0; i < length; ++i) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
    }
    hex[2 * length] = 0;
    (void)hex;
    ESP_LOGI("kvm-hid-trace", "%s handle=%u bytes=%s", operation, handle, hex);
}

static int trace_access(uint16_t connection, uint16_t handle,
                        struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)argument;
    for (size_t i = 0; i < traced_count; ++i) {
        if (traced[i].handle != handle) continue;
        uint8_t first = 0;
        bool write = context->op == BLE_GATT_ACCESS_OP_WRITE_CHR;
        if (write && OS_MBUF_PKTLEN(context->om) > 0)
            os_mbuf_copydata(context->om, 0, 1, &first);
        int result = traced[i].callback(connection, handle, context, traced[i].argument);
        uint8_t payload[96];
        size_t length = OS_MBUF_PKTLEN(context->om);
        if (length > sizeof(payload)) length = sizeof(payload);
        if (result == 0 && os_mbuf_copydata(context->om, 0, length, payload) == 0)
            trace_bytes("ATT", handle, payload, length);
        ESP_LOGI("kvm-hid-trace", "access conn=%u handle=%u uuid=%04x op=%u result=%d len=%u value=%u",
                 connection, handle, traced[i].uuid, context->op, result,
                 (unsigned)OS_MBUF_PKTLEN(context->om), first);
        hid_channel_t *channel = hid_gatt_channel_for(connection);
        if (result == 0 && write && channel) {
            if (traced[i].uuid == BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE)
                hid_channel_set_protocol_mode(channel, first);
            if (traced[i].uuid == BLE_SVC_HID_CHR_UUID16_HID_CTRL_PT)
                if (first == 0) (void)hid_channel_release(channel);
        }
        return result;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void on_register(struct ble_gatt_register_ctxt *context, void *argument)
{
    (void)argument;
    if (context->op == BLE_GATT_REGISTER_OP_CHR &&
        ble_uuid_u16(context->chr.svc_def->uuid) == 0x1812 && traced_count < 24) {
        struct ble_gatt_chr_def *definition = (struct ble_gatt_chr_def *)context->chr.chr_def;
        traced[traced_count].handle = context->chr.val_handle;
        traced[traced_count].uuid = ble_uuid_u16(definition->uuid);
        traced[traced_count].callback = definition->access_cb;
        traced[traced_count++].argument = definition->arg;
        definition->access_cb = trace_access;
    }
    if (context->op == BLE_GATT_REGISTER_OP_DSC &&
        ble_uuid_u16(context->dsc.svc_def->uuid) == 0x1812 && traced_count < 24) {
        struct ble_gatt_dsc_def *definition = (struct ble_gatt_dsc_def *)context->dsc.dsc_def;
        traced[traced_count].handle = context->dsc.handle;
        traced[traced_count].uuid = ble_uuid_u16(definition->uuid);
        traced[traced_count].callback = definition->access_cb;
        traced[traced_count++].argument = definition->arg;
        definition->access_cb = trace_access;
    }
    if (context->op != BLE_GATT_REGISTER_OP_CHR) return;
    uint16_t uuid = ble_uuid_u16(context->chr.chr_def->uuid);
    if (uuid == BLE_SVC_HID_CHR_UUID16_RPT) {
        if (report_characteristic_index++ == 0)
            keyboard_handle = context->chr.val_handle;
        else
            led_handle = context->chr.val_handle;
    } else if (uuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP) {
        boot_keyboard_handle = context->chr.val_handle;
    }
}

static int send_report(void *unused, uint16_t connection_handle, uint8_t report_id,
                       const uint8_t *bytes, size_t length)
{
    (void)unused;
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (!channel || !channel->encrypted || !channel->subscribed[report_id]) return -1;
    uint16_t handle = report_id == HID_REPORT_KEYBOARD ? keyboard_handle :
                      report_id == HID_REPORT_BOOT_KEYBOARD ? boot_keyboard_handle : 0;
    if (handle == 0) return -1;
    trace_bytes("input", handle, bytes, length);
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(bytes, length);
    if (!packet) return -1;
    return ble_gatts_notify_custom(connection_handle, handle, packet);
}

int hid_gatt_register(void)
{
    for (size_t index = 0; index < DOUBTECH_HID_MAX_CONNECTIONS; ++index)
        hid_channel_init(&channels[index], send_report, NULL);

    ble_hs_cfg.gatts_register_cb = on_register;
    ble_svc_bas_init();
    int result = ble_gatts_count_cfg(device_info_services);
    if (result != 0) return result;
    result = ble_gatts_add_svcs(device_info_services);
    if (result != 0) return result;

    struct ble_svc_hid_params parameters = {0};
    static const uint8_t info[] = {0x11, 0x01, 0x00, 0x03};
    memcpy(&parameters.hid_info, info, sizeof(info));
    memcpy(parameters.report_map, hid_report_map, hid_report_map_len);
    parameters.report_map_len = (uint8_t)hid_report_map_len;
    /* External Report Reference names the characteristic, not its service. */
    parameters.external_rpt_ref = 0x2a19;
    parameters.proto_mode_present = 1;
    parameters.proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;
    parameters.kbd_inp_present = 1;
    parameters.kbd_out_present = 1;
    parameters.rpts[0].id = HID_REPORT_KEYBOARD;
    parameters.rpts[0].type = BLE_SVC_HID_RPT_TYPE_INPUT;
    parameters.rpts[0].len = HID_KEYBOARD_REPORT_LEN;
    parameters.rpts[1].id = HID_REPORT_KEYBOARD;
    parameters.rpts[1].type = BLE_SVC_HID_RPT_TYPE_OUTPUT;
    parameters.rpts[1].len = 1;
    parameters.rpts_len = 2;
    result = ble_svc_hid_add(parameters);
    if (result != 0) return result;
    ble_svc_hid_init();
    return 0;
}

hid_channel_t *hid_gatt_channel(void) { return &channels[0]; }

hid_channel_t *hid_gatt_channel_at(uint8_t slot)
{ return slot >= 1 && slot <= DOUBTECH_HID_MAX_CONNECTIONS ? &channels[slot - 1] : NULL; }

hid_channel_t *hid_gatt_channel_for(uint16_t connection_handle)
{
    for (size_t index = 0; index < DOUBTECH_HID_MAX_CONNECTIONS; ++index)
        if (channels[index].connected && channels[index].connection_handle == connection_handle)
            return &channels[index];
    return NULL;
}

size_t hid_gatt_connection_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < DOUBTECH_HID_MAX_CONNECTIONS; ++index)
        if (channels[index].connected) ++count;
    return count;
}

bool hid_gatt_on_connect(uint16_t connection_handle)
{
    if (hid_gatt_channel_for(connection_handle)) return false;
    for (size_t index = 0; index < DOUBTECH_HID_MAX_CONNECTIONS; ++index)
        if (!channels[index].connected) {
            hid_channel_connected(&channels[index], connection_handle);
            return true;
        }
    return false;
}

void hid_gatt_on_disconnect(uint16_t connection_handle)
{
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (channel) hid_channel_disconnected(channel);
}

void hid_gatt_on_encryption(uint16_t connection_handle, bool encrypted)
{
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (channel) hid_channel_encrypted(channel, encrypted);
}

void hid_gatt_on_subscribe(uint16_t connection_handle, uint16_t value_handle, bool enabled)
{
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (!channel) return;
    if (value_handle == keyboard_handle)
        hid_channel_subscribed(channel, HID_REPORT_KEYBOARD, enabled);
    if (value_handle == boot_keyboard_handle)
        hid_channel_subscribed(channel, HID_REPORT_BOOT_KEYBOARD, enabled);
}
