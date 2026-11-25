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
 * a link, or while another connection attempt is pending; ESP_ERR_NOT_FOUND
 * when nothing is stored. The retry schedule belongs to the caller. */
esp_err_t central_connect_stored(uint8_t kind);

#endif
