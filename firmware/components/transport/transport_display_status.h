/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the serialized firmware status projection for the device display.
 * It copies confirmed USB, router, pairing, and authenticated HID facts;
 * unavailable update, touch, and panel sources remain disabled. */
#ifndef ESP32_KVM_TRANSPORT_DISPLAY_STATUS_H
#define ESP32_KVM_TRANSPORT_DISPLAY_STATUS_H

#include "transport_core.h"
#include "display_model.h"

/** Builds one fail-closed display status on the USB worker at monotonic now_ms. */
void kvm_transport_display_status(const kvm_transport_core_t *core,
                                  const kvm_router_t *router, bool usb_connected,
                                  bool hid_ready, uint64_t now_ms,
                                  kvm_display_status_t *status);

#endif
