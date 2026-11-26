/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Holds the composite USB descriptors as plain byte arrays with named values so
 * the same bytes are testable on the host and handed unchanged to TinyUSB. The
 * layout and intervals follow the stage 3 design, Decision 1. */
#include "usb_descriptors.h"
#include <string.h>

/* Descriptor types and class codes. */
#define DT_DEVICE 0x01
#define DT_CONFIG 0x02
#define DT_INTERFACE 0x04
#define DT_ENDPOINT 0x05
#define DT_IAD 0x0B
#define DT_HID 0x21
#define DT_HID_REPORT 0x22
#define DT_CS_INTERFACE 0x24

#define CLASS_CDC 0x02
#define CLASS_HID 0x03
#define CLASS_CDC_DATA 0x0A
#define CLASS_MISC 0xEF

#define EP_INTERRUPT 0x03
#define EP_BULK 0x02

#define LE16(v) (uint8_t)((v) & 0xFF), (uint8_t)(((v) >> 8) & 0xFF)

#define KEYBOARD_REPORT_LEN 65
#define POINTER_REPORT_LEN 100
#define CONFIG_TOTAL_LEN 125

#define CONFIG_HEADER(total) 9, DT_CONFIG, LE16(total), KVM_USB_ITF_COUNT, 1, 0, 0xA0, 125
#define ITF(num, eps, cls, sub, proto) 9, DT_INTERFACE, (num), 0, (eps), (cls), (sub), (proto), 0
#define HID_DESC(report_len) 9, DT_HID, LE16(0x0111), 0, 1, DT_HID_REPORT, LE16(report_len)
#define EP(addr, attr, size, interval) 7, DT_ENDPOINT, (addr), (attr), LE16(size), (interval)

void kvm_usb_device_descriptor(uint16_t vid, uint16_t pid, uint8_t out[18])
{
    const uint8_t d[18] = {
        18, DT_DEVICE, LE16(0x0200),
        CLASS_MISC, 0x02, 0x01, /* IAD composite */
        64,                     /* EP0 */
        LE16(vid), LE16(pid), LE16(KVM_USB_BCD_DEVICE),
        KVM_USB_STR_MANUFACTURER, KVM_USB_STR_PRODUCT, KVM_USB_STR_SERIAL, 1,
    };
    memcpy(out, d, sizeof(d));
}

const uint8_t kvm_usb_config_descriptor[] = {
    CONFIG_HEADER(CONFIG_TOTAL_LEN),
    /* Interface 0: boot keyboard. */
    ITF(KVM_USB_ITF_KEYBOARD, 1, CLASS_HID, 0x01, 0x01),
    HID_DESC(KEYBOARD_REPORT_LEN),
    EP(KVM_USB_EP_KEYBOARD_IN, EP_INTERRUPT, 8, 1),
    /* Interface 1: mouse and consumer control. */
    ITF(KVM_USB_ITF_POINTER, 1, CLASS_HID, 0x00, 0x00),
    HID_DESC(POINTER_REPORT_LEN),
    EP(KVM_USB_EP_POINTER_IN, EP_INTERRUPT, 16, 1),
    /* Interface association: CDC-ACM function = interfaces 2 and 3. */
    8, DT_IAD, KVM_USB_ITF_CDC_COMM, 2, CLASS_CDC, 0x02, 0x00, 0,
    ITF(KVM_USB_ITF_CDC_COMM, 1, CLASS_CDC, 0x02, 0x00),
    5, DT_CS_INTERFACE, 0x00, LE16(0x0110),                   /* header, CDC 1.10 */
    5, DT_CS_INTERFACE, 0x01, 0x00, KVM_USB_ITF_CDC_DATA,     /* call management */
    4, DT_CS_INTERFACE, 0x02, 0x02,                           /* ACM: line coding and serial state, no Send_Break */
    5, DT_CS_INTERFACE, 0x06, KVM_USB_ITF_CDC_COMM, KVM_USB_ITF_CDC_DATA, /* union */
    EP(KVM_USB_EP_CDC_NOTIF_IN, EP_INTERRUPT, 8, 16),
    ITF(KVM_USB_ITF_CDC_DATA, 2, CLASS_CDC_DATA, 0x00, 0x00),
    EP(KVM_USB_EP_CDC_OUT, EP_BULK, 64, 0),
    EP(KVM_USB_EP_CDC_IN, EP_BULK, 64, 0),
};
const size_t kvm_usb_config_descriptor_len = sizeof(kvm_usb_config_descriptor);

/* Boot keyboard: 8 input bytes (modifiers, reserved, six keys), 5 LED output bits + 3 pad. No Report ID. */
const uint8_t kvm_usb_keyboard_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,       /* Generic Desktop, Keyboard, Application */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,       /* Key codes: modifiers E0..E7 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,       /* reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,       /* LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00, /* six keys */
    0xC0,
};
const size_t kvm_usb_keyboard_report_desc_len = sizeof(kvm_usb_keyboard_report_desc);

/* Report ID 1 mouse (7 data bytes): 5 buttons + 3 pad, X/Y int16, wheel int8, AC Pan int8.
 * Report ID 2 consumer control: one 16-bit usage. */
const uint8_t kvm_usb_pointer_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, KVM_USB_REPORT_ID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, KVM_USB_REPORT_ID_CONSUMER,
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};
const size_t kvm_usb_pointer_report_desc_len = sizeof(kvm_usb_pointer_report_desc);

_Static_assert(sizeof(kvm_usb_config_descriptor) == CONFIG_TOTAL_LEN, "wTotalLength must equal the array size");
_Static_assert(sizeof(kvm_usb_keyboard_report_desc) == KEYBOARD_REPORT_LEN, "keyboard wDescriptorLength must equal the array size");
_Static_assert(sizeof(kvm_usb_pointer_report_desc) == POINTER_REPORT_LEN, "pointer wDescriptorLength must equal the array size");

bool kvm_usb_config_parse(const uint8_t *desc, size_t len, kvm_usb_config_summary_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (desc == NULL || len < 9 || desc[0] != 9 || desc[1] != DT_CONFIG) return false;
    size_t total = (size_t)desc[2] | ((size_t)desc[3] << 8);
    if (total < 9 || total > len) return false;

    kvm_usb_config_summary_t s;
    memset(&s, 0, sizeof(s));
    bool iad_pending = false;
    uint8_t iad_first = 0, iad_count = 0;
    size_t off = 9;
    while (off < total) {
        size_t remaining = total - off;
        if (remaining < 2) return false;
        uint8_t blen = desc[off];
        if (blen < 2 || blen > remaining) return false;
        const uint8_t *d = desc + off;
        if (d[1] == DT_INTERFACE) {
            if (blen < 9) return false;
            s.interfaces++;
            if (iad_pending && d[5] == CLASS_CDC && d[2] == iad_first && iad_count == 2) s.iad_before_cdc = true;
            iad_pending = false;
        } else if (d[1] == DT_ENDPOINT) {
            if (blen < 7) return false;
            if (d[2] & 0x80) s.in_endpoints++;
            else s.out_endpoints++;
        } else if (d[1] == DT_IAD) {
            if (blen < 8) return false;
            iad_pending = true;
            iad_first = d[2];
            iad_count = d[3];
        }
        off += blen;
    }
    s.total_length_ok = (total == len);
    *out = s;
    return true;
}
