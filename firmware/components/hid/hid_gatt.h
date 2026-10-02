/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes one composite HID-over-GATT service and its connection-bound event
 * hooks to the firmware BLE owner. Registration never starts advertising or
 * arms input; the owner supplies authenticated link and CCCD events. */
#ifndef ESP32_KVM_HID_GATT_H
#define ESP32_KVM_HID_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_report.h"

/** Registers the HID service before the NimBLE GATT server starts. */
int hid_gatt_register(void);
/** Returns the single guest channel, initially disconnected and disarmed. */
hid_channel_t *hid_gatt_channel(void);
/** Binds one BLE connection; a second simultaneous guest is refused here. */
bool hid_gatt_on_connect(uint16_t connection_handle);
/** Clears the bound connection and held reports. */
void hid_gatt_on_disconnect(uint16_t connection_handle);
/** Applies the NimBLE link's encryption state to the bound connection. */
void hid_gatt_on_encryption(uint16_t connection_handle, bool encrypted);
/** Applies a per-connection notification subscription from a GAP event. */
void hid_gatt_on_subscribe(uint16_t connection_handle, uint16_t value_handle, bool enabled);

#endif
