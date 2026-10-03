/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Registers the HID, Battery and Device Information services with keyboard,
 * mouse, consumer, LED and Boot Protocol reports. Up to three peers have separate
 * encryption, CCCD, protocol, and held-report state; every notification
 * targets exactly its channel's BLE connection handle. */
#include "hid_gatt.h"

#include <string.h>

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

enum attribute {
    ATTR_HID_INFO = 1,
    ATTR_REPORT_MAP,
    ATTR_CONTROL_POINT,
    ATTR_PROTOCOL_MODE,
    ATTR_KEYBOARD_INPUT,
    ATTR_MOUSE_INPUT,
    ATTR_CONSUMER_INPUT,
    ATTR_KEYBOARD_OUTPUT,
    ATTR_BOOT_KEYBOARD_INPUT,
    ATTR_BOOT_KEYBOARD_OUTPUT,
    ATTR_BOOT_MOUSE_INPUT,
    ATTR_KEYBOARD_REFERENCE,
    ATTR_MOUSE_REFERENCE,
    ATTR_CONSUMER_REFERENCE,
    ATTR_LED_REFERENCE,
    ATTR_BATTERY_REFERENCE,
    ATTR_BATTERY_LEVEL,
    ATTR_PNP_ID
};

static hid_channel_t channels[HID_GATT_MAX_CONNECTIONS];
static uint16_t keyboard_handle;
static uint16_t mouse_handle;
static uint16_t consumer_handle;
static uint16_t led_handle;
static uint16_t boot_keyboard_handle;
static uint16_t boot_mouse_handle;

static int access_attribute(uint16_t connection_handle, uint16_t attribute_handle,
                            struct ble_gatt_access_ctxt *context, void *argument);

static struct ble_gatt_dsc_def keyboard_reference[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
     .access_cb = access_attribute, .arg = (void *)ATTR_KEYBOARD_REFERENCE},
    {0}
};
static struct ble_gatt_dsc_def mouse_reference[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
     .access_cb = access_attribute, .arg = (void *)ATTR_MOUSE_REFERENCE},
    {0}
};
static struct ble_gatt_dsc_def consumer_reference[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
     .access_cb = access_attribute, .arg = (void *)ATTR_CONSUMER_REFERENCE},
    {0}
};
static struct ble_gatt_dsc_def led_reference[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
     .access_cb = access_attribute, .arg = (void *)ATTR_LED_REFERENCE},
    {0}
};
static struct ble_gatt_dsc_def report_map_reference[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2907), .att_flags = BLE_ATT_F_READ,
     .access_cb = access_attribute, .arg = (void *)ATTR_BATTERY_REFERENCE},
    {0}
};

static const struct ble_gatt_chr_def characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2a4a), .access_cb = access_attribute,
     .arg = (void *)ATTR_HID_INFO, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(0x2a4b), .access_cb = access_attribute,
     .arg = (void *)ATTR_REPORT_MAP, .descriptors = report_map_reference,
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(0x2a4c), .access_cb = access_attribute,
     .arg = (void *)ATTR_CONTROL_POINT,
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
    {.uuid = BLE_UUID16_DECLARE(0x2a4e), .access_cb = access_attribute,
     .arg = (void *)ATTR_PROTOCOL_MODE,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
              BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC},
    {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_attribute,
     .arg = (void *)ATTR_KEYBOARD_INPUT, .descriptors = keyboard_reference,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
     .val_handle = &keyboard_handle},
    {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_attribute,
     .arg = (void *)ATTR_MOUSE_INPUT, .descriptors = mouse_reference,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
     .val_handle = &mouse_handle},
    {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_attribute,
     .arg = (void *)ATTR_CONSUMER_INPUT, .descriptors = consumer_reference,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
     .val_handle = &consumer_handle},
    {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_attribute,
     .arg = (void *)ATTR_KEYBOARD_OUTPUT, .descriptors = led_reference,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
              BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
     .val_handle = &led_handle},
    {.uuid = BLE_UUID16_DECLARE(0x2a22), .access_cb = access_attribute,
     .arg = (void *)ATTR_BOOT_KEYBOARD_INPUT,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
     .val_handle = &boot_keyboard_handle},
    {.uuid = BLE_UUID16_DECLARE(0x2a32), .access_cb = access_attribute,
     .arg = (void *)ATTR_BOOT_KEYBOARD_OUTPUT,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
              BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC},
    {.uuid = BLE_UUID16_DECLARE(0x2a33), .access_cb = access_attribute,
     .arg = (void *)ATTR_BOOT_MOUSE_INPUT,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
     .val_handle = &boot_mouse_handle},
    {0}
};

static const struct ble_gatt_chr_def battery_characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2a19), .access_cb = access_attribute,
     .arg = (void *)ATTR_BATTERY_LEVEL,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
    {0}
};

static const struct ble_gatt_chr_def device_information_characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2a50), .access_cb = access_attribute,
     .arg = (void *)ATTR_PNP_ID, .flags = BLE_GATT_CHR_F_READ},
    {0}
};

static const struct ble_gatt_svc_def services[];
static const struct ble_gatt_svc_def *battery_include[] = {&services[1], NULL};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
     .includes = battery_include, .characteristics = characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180f),
     .characteristics = battery_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180a),
     .characteristics = device_information_characteristics},
    {0}
};

static int append(struct ble_gatt_access_ctxt *context, const uint8_t *bytes, size_t length)
{
    return os_mbuf_append(context->om, bytes, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int read_attribute(hid_channel_t *channel, struct ble_gatt_access_ctxt *context,
                          enum attribute attribute)
{
    static const uint8_t info[] = {0x11, 0x01, 0x00, 0x02};
    static const uint8_t keyboard_ref[] = {HID_REPORT_KEYBOARD, 1};
    static const uint8_t mouse_ref[] = {HID_REPORT_MOUSE, 1};
    static const uint8_t consumer_ref[] = {HID_REPORT_CONSUMER, 1};
    static const uint8_t led_ref[] = {HID_REPORT_KEYBOARD, 2};
    static const uint8_t battery_ref[] = {0x19, 0x2a};
    /* The USB-powered board has no battery to drain; report available power. */
    static const uint8_t usb_power_level = 100;
    /* Development board's USB-IF VID:PID, already reported by its native USB
       Serial/JTAG interface. Replace with an assigned product ID for release. */
    static const uint8_t pnp_id[] = {0x02, 0x3a, 0x30, 0x01, 0x10, 0x00, 0x01};
    static const uint8_t zero_keyboard[HID_KEYBOARD_REPORT_LEN] = {0};
    uint8_t mouse[HID_MOUSE_REPORT_LEN] = {0};
    uint8_t consumer[HID_CONSUMER_REPORT_LEN] = {0};
    switch (attribute) {
    case ATTR_HID_INFO: return append(context, info, sizeof(info));
    case ATTR_REPORT_MAP: return append(context, hid_report_map, hid_report_map_len);
    case ATTR_PROTOCOL_MODE: return append(context, &channel->protocol_mode, 1);
    case ATTR_KEYBOARD_INPUT:
    case ATTR_BOOT_KEYBOARD_INPUT:
        return append(context, channel->armed ? channel->keyboard : zero_keyboard,
                      HID_KEYBOARD_REPORT_LEN);
    case ATTR_MOUSE_INPUT:
        mouse[0] = channel->armed ? channel->mouse_buttons : 0;
        return append(context, mouse, sizeof(mouse));
    case ATTR_CONSUMER_INPUT:
        if (channel->armed) {
            consumer[0] = (uint8_t)channel->consumer_usage;
            consumer[1] = (uint8_t)(channel->consumer_usage >> 8);
        }
        return append(context, consumer, sizeof(consumer));
    case ATTR_KEYBOARD_OUTPUT: return append(context, &channel->keyboard_leds, 1);
    case ATTR_BOOT_KEYBOARD_OUTPUT: return append(context, &channel->keyboard_leds, 1);
    case ATTR_BOOT_MOUSE_INPUT: {
        uint8_t boot_mouse[3] = {channel->armed ? (uint8_t)(channel->mouse_buttons & 0x07u) : 0, 0, 0};
        return append(context, boot_mouse, sizeof(boot_mouse));
    }
    case ATTR_KEYBOARD_REFERENCE: return append(context, keyboard_ref, sizeof(keyboard_ref));
    case ATTR_MOUSE_REFERENCE: return append(context, mouse_ref, sizeof(mouse_ref));
    case ATTR_CONSUMER_REFERENCE: return append(context, consumer_ref, sizeof(consumer_ref));
    case ATTR_LED_REFERENCE: return append(context, led_ref, sizeof(led_ref));
    case ATTR_BATTERY_REFERENCE: return append(context, battery_ref, sizeof(battery_ref));
    case ATTR_BATTERY_LEVEL: return append(context, &usb_power_level, 1);
    case ATTR_PNP_ID: return append(context, pnp_id, sizeof(pnp_id));
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}

static int write_attribute(hid_channel_t *channel, struct ble_gatt_access_ctxt *context,
                           enum attribute attribute)
{
    uint8_t value;
    if (OS_MBUF_PKTLEN(context->om) != 1 ||
        os_mbuf_copydata(context->om, 0, 1, &value) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (attribute) {
    case ATTR_CONTROL_POINT:
        if (value == 0) return hid_channel_release(channel) ? 0 : BLE_ATT_ERR_UNLIKELY;
        if (value == 1) return 0; /* exit suspend remains disarmed */
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    case ATTR_PROTOCOL_MODE:
        return hid_channel_set_protocol_mode(channel, value) ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    case ATTR_KEYBOARD_OUTPUT:
    case ATTR_BOOT_KEYBOARD_OUTPUT:
        return hid_channel_led_output(channel, value) ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    default: return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
}

static int access_attribute(uint16_t connection_handle, uint16_t attribute_handle,
                            struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)attribute_handle;
    enum attribute attribute = (enum attribute)(uintptr_t)argument;
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (attribute != ATTR_HID_INFO && attribute != ATTR_REPORT_MAP &&
        attribute != ATTR_KEYBOARD_REFERENCE && attribute != ATTR_MOUSE_REFERENCE &&
        attribute != ATTR_CONSUMER_REFERENCE && attribute != ATTR_LED_REFERENCE &&
        attribute != ATTR_BATTERY_REFERENCE &&
        attribute != ATTR_BATTERY_LEVEL && attribute != ATTR_PNP_ID &&
        (!channel || !channel->encrypted))
        return BLE_ATT_ERR_INSUFFICIENT_ENC;
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR ||
        context->op == BLE_GATT_ACCESS_OP_READ_DSC) return read_attribute(channel, context, attribute);
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return write_attribute(channel, context, attribute);
    return BLE_ATT_ERR_UNLIKELY;
}

static int send_report(void *unused, uint16_t connection_handle, uint8_t report_id,
                       const uint8_t *bytes, size_t length)
{
    (void)unused;
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    if (!channel || !channel->encrypted ||
        report_id < HID_REPORT_KEYBOARD || report_id > HID_REPORT_BOOT_MOUSE ||
        !channel->subscribed[report_id]) return -1;
    uint16_t value_handle = report_id == HID_REPORT_KEYBOARD ? keyboard_handle :
                            report_id == HID_REPORT_MOUSE ? mouse_handle :
                            report_id == HID_REPORT_CONSUMER ? consumer_handle :
                            report_id == HID_REPORT_BOOT_KEYBOARD ? boot_keyboard_handle :
                            boot_mouse_handle;
    if (value_handle == 0) return -1;
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(bytes, length);
    if (packet == NULL) return -1;
    return ble_gatts_notify_custom(connection_handle, value_handle, packet);
}

int hid_gatt_register(void)
{
    for (size_t i = 0; i < HID_GATT_MAX_CONNECTIONS; ++i)
        hid_channel_init(&channels[i], send_report, NULL);
    int result = ble_gatts_count_cfg(services);
    if (result != 0) return result;
    return ble_gatts_add_svcs(services);
}

hid_channel_t *hid_gatt_channel(void) { return &channels[0]; }

hid_channel_t *hid_gatt_channel_at(uint8_t slot)
{ return slot >= 1 && slot <= HID_GATT_MAX_CONNECTIONS ? &channels[slot - 1] : NULL; }

hid_channel_t *hid_gatt_channel_for(uint16_t connection_handle)
{
    for (size_t i = 0; i < HID_GATT_MAX_CONNECTIONS; ++i)
        if (channels[i].connected && channels[i].connection_handle == connection_handle)
            return &channels[i];
    return NULL;
}

size_t hid_gatt_connection_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < HID_GATT_MAX_CONNECTIONS; ++i)
        if (channels[i].connected) ++count;
    return count;
}

bool hid_gatt_on_connect(uint16_t connection_handle)
{
    if (hid_gatt_channel_for(connection_handle)) return false;
    for (size_t i = 0; i < HID_GATT_MAX_CONNECTIONS; ++i)
        if (!channels[i].connected) {
            hid_channel_connected(&channels[i], connection_handle);
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
    if (value_handle == keyboard_handle) hid_channel_subscribed(channel, HID_REPORT_KEYBOARD, enabled);
    if (value_handle == mouse_handle) hid_channel_subscribed(channel, HID_REPORT_MOUSE, enabled);
    if (value_handle == consumer_handle) hid_channel_subscribed(channel, HID_REPORT_CONSUMER, enabled);
    if (value_handle == boot_keyboard_handle)
        hid_channel_subscribed(channel, HID_REPORT_BOOT_KEYBOARD, enabled);
    if (value_handle == boot_mouse_handle)
        hid_channel_subscribed(channel, HID_REPORT_BOOT_MOUSE, enabled);
}
