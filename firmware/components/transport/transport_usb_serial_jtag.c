/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Bridges binary framed routing traffic through ESP32-S3 USB Serial/JTAG CDC.
 * One worker serializes transport and router calls, rotates the session on
 * disconnect, and ticks the fail-local lease even when USB input is idle. */
#include "transport_usb_serial_jtag.h"
#include "transport_core.h"
#include "router_hid_bridge.h"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "ESP32 KVM binary CDC protocol cannot share USB Serial/JTAG with the IDF console"
#endif
#if defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) && CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "ESP32 KVM binary CDC protocol cannot share USB Serial/JTAG with secondary logs"
#endif
#if defined(CONFIG_ESP_CONSOLE_USB_CDC) && CONFIG_ESP_CONSOLE_USB_CDC
#error "ESP32 KVM M0 loopback reserves native USB for binary Serial/JTAG CDC"
#endif

#define KVM_USB_BUFFER_SIZE 1024
#define KVM_USB_TASK_STACK 4096

static kvm_transport_core_t core;
static kvm_router_t router;
static bool started;

static uint64_t now_ms(void *context)
{
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t new_session(void)
{
    uint64_t session;
    do { session = ((uint64_t)esp_random() << 32) | esp_random(); } while (!session);
    return session;
}

static void send_binary(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    size_t at = 0;
    while (at < length) {
        int written = usb_serial_jtag_write_bytes(bytes + at, length - at, pdMS_TO_TICKS(50));
        if (written <= 0) {
            kvm_transport_core_reset(&core);
            return;
        }
        at += (size_t)written;
    }
}

static void usb_worker(void *context)
{
    (void)context;
    uint8_t bytes[128];
    bool was_connected = false;
    for (;;) {
        bool connected = usb_serial_jtag_is_connected();
        if (!connected) {
            if (was_connected) kvm_transport_core_reset(&core);
            was_connected = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!was_connected) {
            (void)kvm_transport_core_init(&core, "esp32-kvm-s3", "0.1.0-m1",
                                          new_session(), send_binary, NULL);
            kvm_transport_core_bind_router(&core, &router, now_ms, NULL);
            was_connected = true;
        }
        int read = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        if (read > 0) kvm_transport_core_feed(&core, bytes, (size_t)read);
        kvm_transport_core_tick(&core);
    }
}

esp_err_t kvm_transport_usb_serial_jtag_start(void)
{
    if (started) return ESP_ERR_INVALID_STATE;
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = KVM_USB_BUFFER_SIZE,
        .tx_buffer_size = KVM_USB_BUFFER_SIZE,
    };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK) return result;
    kvm_router_init(&router, kvm_router_hid_output(), NULL);
    if (xTaskCreate(usb_worker, "kvm_usb_loopback", KVM_USB_TASK_STACK, NULL, 10, NULL) != pdPASS) {
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
