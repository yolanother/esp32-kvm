/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes the thread-safe HID output adapter for the serialized USB routing
 * actor (all BLE work is marshaled to the NimBLE host loop) and the copy-only
 * conversion of central input-device reports into hub queue messages. */
#ifndef ESP32_KVM_ROUTER_HID_BRIDGE_H
#define ESP32_KVM_ROUTER_HID_BRIDGE_H

#include "router.h"
#include "transport_hub.h"
#include "doubtech_hid_central.h"

/** Builds callback operations for router slots one to three; local slot zero has no output. */
kvm_router_output_t kvm_router_hid_output(void);
/** Copies one central report into a hub message: keyboard and mouse reports
 * become INPUT, a lost report becomes LOST. False for NULL or an unknown kind.
 * Safe on the NimBLE host thread: it touches nothing but its arguments. */
bool kvm_router_hid_central_msg(const doubtech_hid_central_input_t *in, kvm_hub_msg_t *out);

#endif
