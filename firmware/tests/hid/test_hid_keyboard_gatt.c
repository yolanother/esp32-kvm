/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Tests the composite GATT service registration and encrypted guests'
 * per-peer encryption, CCCD, held state and notification isolation across
 * three simultaneous connections against NimBLE API stand-ins. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "hid_gatt.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

static const struct ble_gatt_svc_def *registered;
static uint16_t notified_connection;
static uint16_t notified_handle;
static uint8_t notified_bytes[HID_KEYBOARD_REPORT_LEN];
static size_t notified_length;
static unsigned notified_count;

int os_mbuf_append(struct os_mbuf *buffer, const void *data, size_t length)
{
    if (buffer->len + length > sizeof(buffer->data)) return -1;
    memcpy(buffer->data + buffer->len, data, length);
    buffer->len += length;
    return 0;
}

int os_mbuf_copydata(const struct os_mbuf *buffer, size_t offset, size_t length, void *out)
{
    if (offset + length > buffer->len) return -1;
    memcpy(out, buffer->data + offset, length);
    return 0;
}

struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, size_t length)
{
    struct os_mbuf *packet = calloc(1, sizeof(*packet));
    if (packet == NULL || os_mbuf_append(packet, data, length) != 0) {
        free(packet);
        return NULL;
    }
    return packet;
}

int ble_gatts_notify_custom(uint16_t connection_handle, uint16_t value_handle, struct os_mbuf *packet)
{
    notified_connection = connection_handle;
    notified_handle = value_handle;
    notified_length = packet->len;
    memcpy(notified_bytes, packet->data, packet->len);
    notified_count++;
    free(packet);
    return 0;
}

int ble_gatts_count_cfg(const struct ble_gatt_svc_def *services)
{
    registered = services;
    return 0;
}

int ble_gatts_add_svcs(const struct ble_gatt_svc_def *services)
{
    assert(services == registered && services[0].type == BLE_GATT_SVC_TYPE_PRIMARY);
    uint16_t handle = 10;
    for (const struct ble_gatt_chr_def *chr = services[0].characteristics; chr->uuid; chr++)
        if (chr->val_handle) *chr->val_handle = handle++;
    return 0;
}

static const struct ble_gatt_chr_def *characteristic(unsigned index)
{
    return &registered[0].characteristics[index];
}

static int access(unsigned index, uint16_t connection, uint8_t operation,
                  const uint8_t *write, size_t length, struct os_mbuf *result)
{
    struct os_mbuf packet = {0};
    if (write != NULL) os_mbuf_append(&packet, write, length);
    struct ble_gatt_access_ctxt context = {.op = operation, .om = &packet};
    const struct ble_gatt_chr_def *chr = characteristic(index);
    int code = chr->access_cb(connection, 0, &context, chr->arg);
    if (result != NULL) *result = packet;
    return code;
}

/* Exercise the actual keyboard profile, not a mocked SDK service builder. */
int main(void)
{
    assert(hid_gatt_register() == 0);
    assert((uintptr_t)registered[0].uuid == 0x1812 && !registered[0].includes);
    assert((uintptr_t)registered[1].uuid == 0x180f);
    assert((uintptr_t)registered[2].uuid == 0x180a && !registered[3].type);
    assert(!characteristic(8)->uuid); /* No mouse or consumer characteristics. */
    assert(!characteristic(1)->descriptors); /* No unmapped external battery report. */
    const struct ble_gatt_chr_def *input = characteristic(4);
    assert((input->flags & (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC)) ==
           (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC));
    assert(!(input->flags & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP)));
    struct os_mbuf bytes = {0};
    assert(access(1, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == hid_report_map_len && !memcmp(bytes.data, hid_report_map, bytes.len));
    assert(access(1, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, NULL, 0, NULL) == BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    bytes.len = 0;
    struct ble_gatt_access_ctxt context = {.op = BLE_GATT_ACCESS_OP_READ_CHR, .om = &bytes};
    const struct ble_gatt_chr_def *pnp = &registered[2].characteristics[0];
    assert(pnp->access_cb(17, 0, &context, pnp->arg) == 0);
    const uint8_t expected_pnp[] = {2, 0x3a, 0x30, 1, 0x10, 0, 1};
    assert(bytes.len == sizeof(expected_pnp) && !memcmp(bytes.data, expected_pnp, bytes.len));
    bytes.len = 0;
    const struct ble_gatt_chr_def *battery = registered[1].characteristics;
    assert(battery->access_cb(17, 0, &context, battery->arg) == 0);
    assert(bytes.len == 1 && bytes.data[0] == 100);
    bytes.len = 0;
    context.op = BLE_GATT_ACCESS_OP_READ_DSC;
    const struct ble_gatt_dsc_def *format = battery->descriptors;
    assert(format->access_cb(17, 0, &context, format->arg) == 0);
    const uint8_t expected_format[] = {4, 0, 0xad, 0x27, 1, 0, 0};
    assert(bytes.len == 7 && !memcmp(bytes.data, expected_format, bytes.len));
    assert(hid_gatt_on_connect(17) && hid_gatt_on_connect(18));
    const uint8_t zero[8] = {0}, a[8] = {0, 0, 4}, suspend = 0, resume = 1;
    assert(access(4, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == BLE_ATT_ERR_INSUFFICIENT_ENC);
    bytes.len = 0;
    struct ble_gatt_access_ctxt insecure_ref = {.op = BLE_GATT_ACCESS_OP_READ_DSC, .om = &bytes};
    const struct ble_gatt_dsc_def *insecure_descriptor = input->descriptors;
    assert(insecure_descriptor->access_cb(17, 0, &insecure_ref, insecure_descriptor->arg) == BLE_ATT_ERR_INSUFFICIENT_ENC);
    assert(bytes.len == 0 && !hid_channel_ready(hid_gatt_channel()));
    hid_gatt_on_encryption(17, true);
    for (unsigned i = 4; i <= 5; ++i) {
        bytes.len = 0;
        const struct ble_gatt_dsc_def *ref = characteristic(i)->descriptors;
        assert((uintptr_t)ref->uuid == 0x2908 && (ref->att_flags & BLE_ATT_F_READ_ENC));
        struct ble_gatt_access_ctxt reference_read = {.op = BLE_GATT_ACCESS_OP_READ_DSC, .om = &bytes};
        assert(ref->access_cb(17, 0, &reference_read, ref->arg) == 0);
        assert(bytes.len == 2 && bytes.data[0] == 1 && bytes.data[1] == i - 3);
    }
    /* A suspend during discovery must not poison the link or require a CCCD. */
    assert(access(2, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &suspend, 1, NULL) == 0);
    assert(!hid_gatt_channel()->needs_disconnect);
    assert(access(2, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &resume, 1, NULL) == 0);
    hid_gatt_on_subscribe(17, *input->val_handle, true);
    assert(hid_channel_ready(hid_gatt_channel()) && hid_channel_arm(hid_gatt_channel()));
    assert(notified_count == 1 && notified_length == 8 && !memcmp(notified_bytes, zero, 8));
    assert(hid_channel_keyboard(hid_gatt_channel(), a));
    assert(notified_count == 2 && notified_handle == *input->val_handle && !memcmp(notified_bytes, a, 8));
    assert(access(4, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == 8 && !memcmp(bytes.data, a, 8));
    assert(access(4, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, a, 8, NULL) == BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(!hid_gatt_channel_for(18)->keyboard[2]);
    assert(hid_channel_mouse(hid_gatt_channel(), 0, 0, 0, 0, 0));
    assert(hid_channel_consumer(hid_gatt_channel(), 0));
    assert(notified_count == 2 && !hid_gatt_channel()->needs_disconnect);
    const uint8_t leds = 2, invalid_leds = 0x80, invalid_mode = 2;
    assert(access(5, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &leds, 1, NULL) == 0);
    assert(access(5, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == 1 && bytes.data[0] == 2 && hid_gatt_channel()->keyboard_leds == 2);
    assert(!hid_gatt_channel_for(18)->keyboard_leds);
    assert(access(5, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &invalid_leds, 1, NULL) == BLE_ATT_ERR_VALUE_NOT_ALLOWED);
    assert(access(5, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, a, 8, NULL) == BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    assert(access(3, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &invalid_mode, 1, NULL) == BLE_ATT_ERR_VALUE_NOT_ALLOWED);
    assert(access(2, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &suspend, 1, NULL) == 0);
    assert(notified_count == 3 && !memcmp(notified_bytes, zero, 8) && !hid_gatt_channel()->armed);
    assert(access(4, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == 8 && !memcmp(bytes.data, zero, 8));
    assert(access(2, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &resume, 1, NULL) == 0);
    assert(access(3, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &suspend, 1, NULL) == 0);
    assert(hid_gatt_channel()->protocol_mode == 0 && !hid_channel_ready(hid_gatt_channel()));
    hid_gatt_on_subscribe(17, *characteristic(6)->val_handle, true);
    assert(hid_channel_arm(hid_gatt_channel()));
    assert(hid_channel_keyboard(hid_gatt_channel(), a));
    assert(notified_handle == *characteristic(6)->val_handle && notified_length == 8);
    assert(access(6, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == 8 && !memcmp(bytes.data, a, 8));
    assert(access(7, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &bytes) == 0);
    assert(bytes.len == 1 && bytes.data[0] == leds);
    assert(hid_channel_release(hid_gatt_channel()));
    assert(!memcmp(notified_bytes, zero, 8));
    hid_gatt_on_disconnect(17);
    assert(!hid_gatt_channel_for(17) && hid_gatt_channel_for(18));
    assert(hid_gatt_on_connect(19));
    assert(!hid_gatt_channel_for(19)->encrypted && !hid_gatt_channel_for(19)->keyboard_leds);
    return 0;
}
