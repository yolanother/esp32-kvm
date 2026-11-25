/* Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
 * Starts the one-guest BLE service and binary USB routing transport on ESP32-S3.
 * Startup selects local with no armed HID output; routing requires a validated
 * session, READY encrypted guest, explicit switch, and explicit arm. The
 * Waveshare panel starts only with the explicitly validated board profile.
 * Start order: doubtech_hid_start here owns the NimBLE host; the input-device
 * central starts later from the USB worker, once doubtech_hid_host_synced(). */
#include "esp_log.h"
#include "doubtech_hid.h"
#include "display.h"
#include "display_labels.h"
#include "transport_usb_serial_jtag.h"

/* Advertises as "DoubTech HID"; the NULL bond namespace keeps the shipped firmware's kvm_bonds table so paired computers stay paired. */
static const doubtech_hid_config_t hid_config = {
    .device_name = "DoubTech HID",
    .manufacturer = "DoubTech",
    .model = "DoubTech HID",
    .bond_namespace = NULL,
    .foreign_bond_reserve = 4,
};

/** Starts services; any startup error leaves the routing gate disarmed. */
void app_main(void)
{
    kvm_transport_diagnostics_init();
    esp_err_t hid_result = doubtech_hid_start(&hid_config);
    if (hid_result != ESP_OK) ESP_LOGE("esp32-kvm", "HID startup failed: %d", hid_result);
    esp_err_t labels_result = kvm_display_labels_init();
    if (labels_result != ESP_OK) ESP_LOGE("esp32-kvm", "Display label storage failed: %d", labels_result);
    esp_err_t usb_result = kvm_transport_usb_serial_jtag_start();
    if (usb_result != ESP_OK) ESP_LOGE("esp32-kvm", "USB startup failed: %d", usb_result);
#if CONFIG_KVM_DISPLAY_WAVESHARE_TOUCH_154_VERIFIED
    const bool enable_panel = true;
#else
    const bool enable_panel = false;
#endif
    kvm_display_set_input_fn(kvm_transport_input_touch);
    esp_err_t display_result = kvm_display_start(enable_panel, kvm_transport_button_event,
                                                 kvm_transport_pairing_touch,
                                                 kvm_transport_target_touch);
    if (display_result != ESP_OK) ESP_LOGE("esp32-kvm", "Display/button startup failed: %d", display_result);
}
