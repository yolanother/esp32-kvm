/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Starts the USB Serial/JTAG CDC adapter and serialized router worker. Startup
 * is local and disarmed; host commands and a READY encrypted guest are needed
 * before any HID report can leave the board. */
#ifndef ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H
#define ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H

#include "esp_err.h"

/** Installs fixed USB CDC and starts the disarmed router worker. */
esp_err_t kvm_transport_usb_serial_jtag_start(void);

#endif /* ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H */
