/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes the one-guest, thread-safe HID output adapter for the serialized
 * USB routing actor. All BLE work is marshaled to the NimBLE host loop. */
#ifndef ESP32_KVM_ROUTER_HID_BRIDGE_H
#define ESP32_KVM_ROUTER_HID_BRIDGE_H

#include "router.h"

/** Builds callback operations for router slot one; local slot zero has no output. */
kvm_router_output_t kvm_router_hid_output(void);

#endif
