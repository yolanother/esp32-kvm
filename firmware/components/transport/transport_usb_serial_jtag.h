/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Starts the USB Serial/JTAG CDC adapter and serialized router worker. Startup
 * is local and disarmed; host commands and a READY encrypted guest are needed
 * before any HID report can leave the board. The same worker runs the
 * device-input hub, which routes a paired keyboard and mouse while no host
 * route is armed. */
#ifndef ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H
#define ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H

#include "esp_err.h"
#include "display_model.h"
#include "transport_hub.h"
#include "transport_link.h"
#include <stddef.h>

/** Opt-in trace commands always release routing and close the host session. */
void kvm_transport_diagnostics_init(void);
bool kvm_transport_diagnostics_requested(const uint8_t *bytes, size_t length);
/** Writes the trace over the USB Serial/JTAG link (dump_to with that link). */
void kvm_transport_diagnostics_dump(void);
/** Writes the trace over `link`, 100 ms per write, stopping at a refused chunk. */
void kvm_transport_diagnostics_dump_to(const kvm_transport_link_t *link);

/** Starts the disarmed router worker on an already usable byte link (worker
 * name, priority and stack are fixed). ESP_ERR_INVALID_STATE once started,
 * ESP_ERR_INVALID_ARG for a NULL link or a missing callback. */
esp_err_t kvm_transport_usb_start(const kvm_transport_link_t *link);
/** Installs fixed USB Serial/JTAG CDC, then kvm_transport_usb_start() on its
 * link; the driver is uninstalled again if the worker cannot start. */
esp_err_t kvm_transport_usb_serial_jtag_start(void);
/** Queues a physical button request for the serialized USB/router task. */
void kvm_transport_button_event(kvm_display_event_t event);
/** Queues an exact local touch request without blocking the LVGL thread. */
void kvm_transport_pairing_touch(kvm_display_pair_request_t request);
/** Queues a local/guest card selection: the device-input target while no host
 * session is open, otherwise a selection request to the host. */
void kvm_transport_target_touch(uint8_t slot);
/** Queues one input-device pairing touch (begin, select a displayed row, cancel)
 *  for the worker, which alone calls the BLE central. */
void kvm_transport_input_touch(kvm_display_input_request_t request);
/** Copies one device-input message into the worker's bounded hub queue (depth
 * 16; overflow drops pointer motion first; a LOST message is never dropped).
 * Safe from the NimBLE host thread; ignored before the transport starts. */
void kvm_transport_hub_submit(const kvm_hub_msg_t *msg);

#endif /* ESP32_KVM_TRANSPORT_USB_SERIAL_JTAG_H */
