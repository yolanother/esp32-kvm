/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Tests the composite GATT service registration and one encrypted guest's
 * connection-addressed notification behavior against NimBLE API stand-ins. */
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

int main(void)
{
    assert(hid_gatt_register() == 0);
    assert(registered[1].type == 0);
    for (unsigned index = 4; index <= 6; index++) {
        const struct ble_gatt_chr_def *chr = characteristic(index);
        assert(chr->flags & BLE_GATT_CHR_F_NOTIFY);
        assert(chr->flags & BLE_GATT_CHR_F_READ_ENC);
        assert(chr->descriptors != NULL && chr->descriptors[0].uuid != NULL);
        struct os_mbuf reference = {0};
        struct ble_gatt_access_ctxt descriptor = {.op = BLE_GATT_ACCESS_OP_READ_DSC, .om = &reference};
        assert(chr->descriptors[0].access_cb(17, 0, &descriptor, chr->descriptors[0].arg) == 0);
        assert(reference.len == 2 && reference.data[0] == index - 3 && reference.data[1] == 1);
    }
    struct os_mbuf led_reference = {0};
    struct ble_gatt_access_ctxt descriptor = {.op = BLE_GATT_ACCESS_OP_READ_DSC, .om = &led_reference};
    assert(characteristic(7)->descriptors[0].access_cb(17, 0, &descriptor,
           characteristic(7)->descriptors[0].arg) == 0);
    assert(led_reference.len == 2 && led_reference.data[0] == 1 && led_reference.data[1] == 2);
    assert(!hid_gatt_channel()->armed);
    assert(hid_gatt_on_connect(17));
    assert(!hid_gatt_on_connect(18));
    struct os_mbuf packet = {0};
    assert(access(4, 17, BLE_GATT_ACCESS_OP_READ_CHR, NULL, 0, &packet) == BLE_ATT_ERR_INSUFFICIENT_ENC);
    hid_gatt_on_encryption(17, true);
    hid_gatt_on_subscribe(18, *characteristic(4)->val_handle, true);
    assert(!hid_gatt_channel()->subscribed[HID_REPORT_KEYBOARD]);
    for (unsigned index = 4; index <= 6; index++)
        hid_gatt_on_subscribe(17, *characteristic(index)->val_handle, true);
    assert(hid_channel_arm(hid_gatt_channel()));
    assert(notified_count == 3 && notified_connection == 17);
    uint8_t keys[8] = {0, 0, 4};
    assert(hid_channel_keyboard(hid_gatt_channel(), keys));
    assert(notified_connection == 17 && notified_handle == *characteristic(4)->val_handle);
    assert(notified_length == 8 && notified_bytes[2] == 4);
    uint8_t boot = 0;
    assert(access(3, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &boot, 1, NULL) == BLE_ATT_ERR_VALUE_NOT_ALLOWED);
    uint8_t leds = 3;
    assert(access(7, 17, BLE_GATT_ACCESS_OP_WRITE_CHR, &leds, 1, NULL) == 0);
    assert(hid_gatt_channel()->keyboard_leds == 3);
    hid_gatt_on_encryption(17, false);
    assert(!hid_gatt_channel()->armed);
    hid_gatt_on_disconnect(17);
    assert(!hid_gatt_channel()->connected);
    return 0;
}
