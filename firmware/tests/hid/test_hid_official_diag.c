/* Verify the diagnostic exposes binary device identity and sends an exact
 * eight-byte keyboard report through its registered input characteristic. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "hid_gatt.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "services/hid/ble_svc_hid.h"

struct ble_hs_cfg ble_hs_cfg;
static const struct ble_gatt_svc_def *device_info;
static struct ble_svc_hid_params parameters;
static uint8_t notified[8];
static unsigned notifications;

int os_mbuf_append(struct os_mbuf *buffer, const void *bytes, size_t length)
{
    if (buffer->len + length > sizeof(buffer->data)) return -1;
    memcpy(buffer->data + buffer->len, bytes, length);
    buffer->len += length;
    return 0;
}
int os_mbuf_copydata(const struct os_mbuf *buffer, size_t offset, size_t length, void *out)
{
    if (offset + length > buffer->len) return -1;
    memcpy(out, buffer->data + offset, length);
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *bytes, size_t length)
{
    struct os_mbuf *packet = calloc(1, sizeof(*packet));
    assert(packet && os_mbuf_append(packet, bytes, length) == 0);
    return packet;
}
int ble_gatts_notify_custom(uint16_t connection, uint16_t handle, struct os_mbuf *packet)
{
    assert(connection == 1 && handle == 31 && packet->len == 8);
    memcpy(notified, packet->data, sizeof(notified));
    notifications++;
    free(packet);
    return 0;
}
void ble_svc_bas_init(void) {}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *services)
{
    device_info = services;
    return 0;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *services)
{
    assert(services == device_info);
    return 0;
}
int ble_svc_hid_add(struct ble_svc_hid_params params)
{
    parameters = params;
    return 0;
}
static int builder_access(uint16_t conn, uint16_t handle,
                            struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)conn; (void)handle; (void)context; (void)argument;
    return 0;
}
void ble_svc_hid_init(void)
{
    static struct ble_gatt_chr_def keyboard = {
        .uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = builder_access
    };
    static const struct ble_gatt_svc_def service = {
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812)
    };
    struct ble_gatt_register_ctxt event = {0};
    event.op = BLE_GATT_REGISTER_OP_CHR;
    event.chr.svc_def = &service;
    event.chr.chr_def = &keyboard;
    event.chr.val_handle = 31;
    ble_hs_cfg.gatts_register_cb(&event, NULL);
}
int main(void)
{
    assert(hid_gatt_register() == 0);
    assert(ble_uuid_u16(device_info[0].uuid) == 0x180a);
    const struct ble_gatt_chr_def *pnp = NULL;
    for (const struct ble_gatt_chr_def *chr = device_info[0].characteristics; chr->uuid; chr++)
        if (ble_uuid_u16(chr->uuid) == 0x2a50) pnp = chr;
    assert(pnp && (pnp->flags & BLE_GATT_CHR_F_READ));
    struct os_mbuf bytes = {0};
    struct ble_gatt_access_ctxt read = {.op = BLE_GATT_ACCESS_OP_READ_CHR, .om = &bytes};
    assert(pnp->access_cb(1, 20, &read, pnp->arg) == 0);
    const uint8_t expected[] = {2, 0x3a, 0x30, 1, 0x10, 0, 1};
    assert(bytes.len == sizeof(expected) && memcmp(bytes.data, expected, sizeof(expected)) == 0);
    read.op = BLE_GATT_ACCESS_OP_WRITE_CHR;
    assert(pnp->access_cb(1, 20, &read, pnp->arg) == BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(parameters.hid_info == 0x03000111 && parameters.external_rpt_ref == 0x2a19);
    assert(parameters.report_map_len == hid_report_map_len);
    assert(memcmp(parameters.report_map, hid_report_map, hid_report_map_len) == 0);
    assert(parameters.rpts_len == 2 && parameters.rpts[0].id == 1);
    assert(parameters.rpts[0].type == 1 && parameters.rpts[0].len == 8);
    assert(parameters.rpts[1].type == 2 && parameters.rpts[1].len == 1);
    assert(hid_gatt_on_connect(1));
    hid_gatt_on_encryption(1, true);
    hid_gatt_on_subscribe(1, 31, true);
    hid_channel_t *channel = hid_gatt_channel();
    assert(hid_channel_arm(channel));
    const uint8_t a[8] = {0, 0, 4};
    assert(hid_channel_keyboard(channel, a));
    assert(memcmp(notified, a, sizeof(a)) == 0 && notifications == 2);
    assert(hid_channel_mouse(channel, 0, 0, 0, 0, 0));
    assert(hid_channel_consumer(channel, 0));
    assert(notifications == 2 && channel->armed && !channel->needs_disconnect);
    assert(hid_channel_release(channel));
    const uint8_t zero[8] = {0};
    assert(notifications == 3 && memcmp(notified, zero, sizeof(zero)) == 0);
    return 0;
}
