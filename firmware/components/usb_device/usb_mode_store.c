/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the USB mode store on NVS: open, read or write, commit, close on
 * every call, so no handle outlives a call and every write is durable when the
 * call returns. Corrupt or unknown values never reach the boot as composite. */
#include "usb_mode_store.h"
#include "nvs.h"

#define KVM_USB_NVS_NAMESPACE "kvm_usb"
#define KVM_USB_NVS_KEY_MODE "mode"
#define KVM_USB_NVS_KEY_UNCONF "unconf"

esp_err_t kvm_usb_mode_store_load(uint8_t *mode, uint8_t *unconfirmed)
{
    if (mode == NULL || unconfirmed == NULL) return ESP_ERR_INVALID_ARG;
    *mode = 0;
    *unconfirmed = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KVM_USB_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; /* namespace never written */
    if (err != ESP_OK) return err;
    uint8_t stored_mode = 0, stored_unconf = 0;
    esp_err_t mode_err = nvs_get_u8(handle, KVM_USB_NVS_KEY_MODE, &stored_mode);
    esp_err_t unconf_err = nvs_get_u8(handle, KVM_USB_NVS_KEY_UNCONF, &stored_unconf);
    nvs_close(handle);
    if (mode_err == ESP_ERR_NVS_NOT_FOUND) { stored_mode = 0; mode_err = ESP_OK; }
    if (unconf_err == ESP_ERR_NVS_NOT_FOUND) { stored_unconf = 0; unconf_err = ESP_OK; }
    if (mode_err != ESP_OK) return mode_err;
    if (unconf_err != ESP_OK) return unconf_err;
    *mode = stored_mode == KVM_USB_MODE_COMPOSITE ? (uint8_t)KVM_USB_MODE_COMPOSITE
                                                  : (uint8_t)KVM_USB_MODE_SERIAL_JTAG;
    *unconfirmed = stored_unconf;
    return ESP_OK;
}

static esp_err_t write_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KVM_USB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t kvm_usb_mode_store_set_mode(kvm_usb_mode_t mode)
{
    if (mode != KVM_USB_MODE_SERIAL_JTAG && mode != KVM_USB_MODE_COMPOSITE)
        return ESP_ERR_INVALID_ARG;
    return write_u8(KVM_USB_NVS_KEY_MODE, (uint8_t)mode);
}

esp_err_t kvm_usb_mode_store_set_unconfirmed(uint8_t count)
{
    return write_u8(KVM_USB_NVS_KEY_UNCONF, count);
}
