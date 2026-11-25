/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Component-internal entry points of the NimBLE central adapter. */
#ifndef DTHID_CENTRAL_BLE_H
#define DTHID_CENTRAL_BLE_H

#include <stdint.h>

#include "esp_err.h"

/** Starts one connection attempt (10 s) to the stored device of that kind by
 * its identity address; the link then encrypts with the stored bond, discovers
 * HID and reports CONNECTED. Call on the NimBLE host thread. Refused
 * (ESP_ERR_INVALID_STATE) during a pairing window, while that kind already has
 * a link, while another connection attempt is pending (ours or any other
 * initiator), or when the stored identity is a guest's; ESP_ERR_NOT_FOUND
 * when nothing is stored, ESP_ERR_NO_MEM when the controller has no room.
 * The reconnect schedule in central_ble.c is its only caller. */
esp_err_t central_connect_stored(uint8_t kind);

#endif
