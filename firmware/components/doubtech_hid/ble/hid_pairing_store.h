/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares NVS persistence for opaque HID bond identity tokens. NimBLE keeps
 * cryptographic bond material in its own store; this file stores no BLE keys. */
#ifndef ESP32_KVM_HID_PAIRING_STORE_H
#define ESP32_KVM_HID_PAIRING_STORE_H
#include "esp_err.h"
#include "hid_pairing.h"
/** Loads a versioned token table; absent storage yields an empty table. */
esp_err_t hid_pairing_store_load(hid_pairing_t *state);
/** Atomically commits the current token table; failure must block admission. */
esp_err_t hid_pairing_store_save(const hid_pairing_t *state);
#endif
