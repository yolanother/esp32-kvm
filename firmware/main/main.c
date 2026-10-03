/* Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
 * Starts the one-guest BLE service and binary USB routing transport on ESP32-S3.
 * Startup selects local with no armed HID output; routing requires a validated
 * session, READY encrypted guest, explicit switch, and explicit arm. The
 * Waveshare panel starts only with the explicitly validated board profile. */
#include "esp_log.h"
#include "hid_guest.h"
#include "display.h"
#include "transport_usb_serial_jtag.h"

/** Starts services; any startup error leaves the routing gate disarmed. */
void app_main(void)
{
    esp_err_t hid_result = hid_guest_start();
    if (hid_result != ESP_OK) ESP_LOGE("esp32-kvm", "HID startup failed: %d", hid_result);
    esp_err_t usb_result = kvm_transport_usb_serial_jtag_start();
    if (usb_result != ESP_OK) ESP_LOGE("esp32-kvm", "USB startup failed: %d", usb_result);
#if CONFIG_KVM_DISPLAY_WAVESHARE_TOUCH_154_VERIFIED
    const bool enable_panel = true;
#else
    const bool enable_panel = false;
#endif
    esp_err_t display_result = kvm_display_start(enable_panel, kvm_transport_button_event,
                                                 kvm_transport_pairing_touch);
    if (display_result != ESP_OK) ESP_LOGE("esp32-kvm", "Display/button startup failed: %d", display_result);
}
