/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Starts the USB Serial/JTAG CDC adapter and serialized router worker. Startup
 * is local and disarmed; host commands and a READY encrypted guest are needed
 * before any HID report can leave the board. */
#ifndef ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H
#define ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H

#include "esp_err.h"
#include "display_model.h"

/** Installs fixed USB CDC and starts the disarmed router worker. */
esp_err_t kvm_transport_usb_serial_jtag_start(void);
/** Queues a physical button request for the serialized USB/router task. */
void kvm_transport_button_event(kvm_display_event_t event);
/** Queues an exact local touch request without blocking the LVGL thread. */
void kvm_transport_pairing_touch(kvm_display_pair_request_t request);

#endif /* ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H */
