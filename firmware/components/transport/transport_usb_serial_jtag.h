/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Starts the M0 USB Serial/JTAG CDC loopback adapter. It binds the binary-only
 * transport core to the ESP32-S3's fixed-function native USB controller and
 * leaves every HID route disarmed. The firmware entry point calls this after
 * board identity and recovery are verified. */
#ifndef ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H
#define ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H

#include "esp_err.h"

/** Installs the fixed USB CDC driver and starts a disarmed loopback worker. */
esp_err_t kvm_transport_usb_serial_jtag_start(void);

#endif /* ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H */
