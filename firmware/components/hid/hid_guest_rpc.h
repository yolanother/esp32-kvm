/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares initialization for the bounded HID request bridge that serializes
 * USB worker requests onto NimBLE's host event loop. */
#ifndef ESP32_KVM_HID_GUEST_RPC_H
#define ESP32_KVM_HID_GUEST_RPC_H
#include "esp_err.h"
/** Initializes the single request slot before the BLE host task starts. */
esp_err_t hid_guest_rpc_init(void);
#endif
