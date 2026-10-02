/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exposes opt-in single-guest NimBLE startup for hardware bring-up. Firmware
 * main remains disarmed; callers must verify board recovery and explicitly
 * start this service before it can advertise or accept a guest. */
#ifndef ESP32_KVM_HID_GUEST_H
#define ESP32_KVM_HID_GUEST_H

#include "esp_err.h"

/** Initializes NVS/NimBLE, registers HID, and starts the BLE host task. */
esp_err_t hid_guest_start(void);

#endif
