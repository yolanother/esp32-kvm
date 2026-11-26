/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the composite USB descriptors (boot keyboard, mouse + consumer
 * control, CDC-ACM) as pure byte data, plus a bounds-checked structural parser
 * used by the tests. No TinyUSB or ESP-IDF headers live here. */
#ifndef ESP32_KVM_USB_DESCRIPTORS_H
#define ESP32_KVM_USB_DESCRIPTORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KVM_USB_ITF_KEYBOARD 0
#define KVM_USB_ITF_POINTER 1
#define KVM_USB_ITF_CDC_COMM 2
#define KVM_USB_ITF_CDC_DATA 3
#define KVM_USB_ITF_COUNT 4

#define KVM_USB_EP_KEYBOARD_IN 0x81
#define KVM_USB_EP_POINTER_IN 0x82
#define KVM_USB_EP_CDC_NOTIF_IN 0x83
#define KVM_USB_EP_CDC_OUT 0x04
#define KVM_USB_EP_CDC_IN 0x84

#define KVM_USB_REPORT_ID_MOUSE 1
#define KVM_USB_REPORT_ID_CONSUMER 2

#define KVM_USB_BCD_DEVICE 0x0100
#define KVM_USB_ESPRESSIF_VID 0x303A

/* String descriptor indexes referenced by the device descriptor. */
#define KVM_USB_STR_MANUFACTURER 1
#define KVM_USB_STR_PRODUCT 2
#define KVM_USB_STR_SERIAL 3

/* Writes the 18-byte device descriptor for the given VID/PID. */
void kvm_usb_device_descriptor(uint16_t vid, uint16_t pid, uint8_t out[18]);

extern const uint8_t kvm_usb_config_descriptor[];
extern const size_t kvm_usb_config_descriptor_len;
extern const uint8_t kvm_usb_keyboard_report_desc[];
extern const size_t kvm_usb_keyboard_report_desc_len;
extern const uint8_t kvm_usb_pointer_report_desc[];
extern const size_t kvm_usb_pointer_report_desc_len;

typedef struct {
    uint8_t interfaces;
    uint8_t in_endpoints;
    uint8_t out_endpoints;
    bool iad_before_cdc;
    bool total_length_ok;
} kvm_usb_config_summary_t;

/* Walks a configuration descriptor without reading past len. Returns false (and
 * zeroes out) for null arguments, a header shorter than 9 bytes, a wrong header
 * type, wTotalLength larger than len, or any descriptor with bLength under 2 or
 * running past the end. Trailing bytes after wTotalLength parse but leave
 * total_length_ok false. */
bool kvm_usb_config_parse(const uint8_t *desc, size_t len, kvm_usb_config_summary_t *out);

#endif
