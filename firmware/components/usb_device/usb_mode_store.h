/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the persistent USB mode store (spec Decision 6): NVS namespace
 * "kvm_usb", keys "mode" (u8, 0 serial, 1 composite) and "unconf" (u8, number
 * of unconfirmed composite boots). Each call opens and closes its own handle;
 * setters commit before returning. Nothing here ever erases NVS. The caller
 * must have run nvs_flash_init() (doubtech_hid_start does, before the boot
 * decision); without it every call fails, which the boot reads as serial. */
#ifndef ESP32_KVM_USB_MODE_STORE_H
#define ESP32_KVM_USB_MODE_STORE_H

#include <stdint.h>
#include "esp_err.h"
#include "usb_mode.h"

/** Loads both keys. A missing namespace or key reads as 0 with ESP_OK. A
 *  stored mode other than 0 or 1 reads as 0 (serial). Any other failure (NVS
 *  not initialized, wrong item type, ...) returns that error with both
 *  outputs set to 0, so a caller that ignores the error still sees serial.
 *  NULL outputs return ESP_ERR_INVALID_ARG. */
esp_err_t kvm_usb_mode_store_load(uint8_t *mode, uint8_t *unconfirmed);

/** Writes "mode" (0 or 1 only, else ESP_ERR_INVALID_ARG) and commits. */
esp_err_t kvm_usb_mode_store_set_mode(kvm_usb_mode_t mode);

/** Writes "unconf" and commits. */
esp_err_t kvm_usb_mode_store_set_unconfirmed(uint8_t count);

#endif
