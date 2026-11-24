/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes one composite HID-over-GATT service with up to three isolated
 * connection channels. Registration never starts advertising or arms input;
 * the owner supplies authenticated link and per-peer CCCD events. */
#ifndef ESP32_KVM_HID_GATT_H
#define ESP32_KVM_HID_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_report.h"

/** Source-side channel bound; wire-visible live capacity remains one pending proof. */
#define HID_GATT_MAX_CONNECTIONS 3

/** Registers the HID service before the NimBLE GATT server starts. */
int hid_gatt_register(void);
/** Returns the first routing channel for the existing slot-one bridge. */
hid_channel_t *hid_gatt_channel(void);
/** Returns a one-based channel slot, including an inert disconnected slot. */
hid_channel_t *hid_gatt_channel_at(uint8_t slot);
/** Returns only the connected channel belonging to this handle, or NULL. */
hid_channel_t *hid_gatt_channel_for(uint16_t connection_handle);
/** Counts connected channels without exposing peer addresses or keys. */
size_t hid_gatt_connection_count(void);
/** Binds a connection to the first free channel, rejecting duplicates or overflow. */
bool hid_gatt_on_connect(uint16_t connection_handle);
/** Clears the bound connection and held reports. */
void hid_gatt_on_disconnect(uint16_t connection_handle);
/** Applies the NimBLE link's encryption state to the bound connection. */
void hid_gatt_on_encryption(uint16_t connection_handle, bool encrypted);
/** Applies a per-connection notification subscription from a GAP event. */
void hid_gatt_on_subscribe(uint16_t connection_handle, uint16_t value_handle, bool enabled);

#endif
