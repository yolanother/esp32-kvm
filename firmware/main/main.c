/* Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
 * Starts a deliberately disarmed ESP32-S3 firmware image. Input transport, BLE HID,
 * routing and display initialization are separate bring-up tasks after board verification. */
#include "esp_log.h"

/** Keep the device disarmed until validated transports and routing are implemented. */
void app_main(void)
{
    ESP_LOGI("esp32-kvm", "Scaffold firmware booted; input routing is disabled");
}
