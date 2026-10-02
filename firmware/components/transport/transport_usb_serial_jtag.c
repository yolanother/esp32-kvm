/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Bridges binary framed routing traffic through ESP32-S3 USB Serial/JTAG CDC.
 * One worker serializes transport and router calls, rotates the session on
 * disconnect, and ticks the fail-local lease even when USB input is idle. */
#include "transport_usb_serial_jtag.h"
#include "transport_core.h"
#include "router_hid_bridge.h"
#include "display.h"
#include "hid_guest.h"
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
static TaskHandle_t usb_task;

void kvm_transport_button_event(kvm_display_event_t event)
{
    if (!usb_task) return;
    uint32_t bit = event == KVM_DISPLAY_EMERGENCY_RELEASE ? 2u :
                   event == KVM_DISPLAY_NEXT_REQUEST ? 1u : 0u;
    if (bit) (void)xTaskNotify(usb_task, bit, eSetBits);
}

static void publish_status(bool connected, bool guest_ready)
{
    kvm_display_status_t status = {
        .usb_connected = connected,
        .armed = router.armed,
        .guest_ready = guest_ready,
        .fault = router.fault,
        .selected_slot = router.slot,
        .generation = router.generation,
    };
    kvm_display_post_status(&status);
}

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
    bool guest_ready = false;
    uint64_t last_ready_ms = 0;
    for (;;) {
        uint32_t button_bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &button_bits, 0);
        if (button_bits & 2u) {
            kvm_router_emergency_release(&router);
            kvm_transport_core_reset(&core);
            was_connected = false;
        }
        bool connected = usb_serial_jtag_is_connected();
        uint64_t time_ms = now_ms(NULL);
        if (time_ms < last_ready_ms || time_ms - last_ready_ms >= 1000) {
            guest_ready = hid_guest_request_ready();
            last_ready_ms = time_ms;
        }
        if (!connected) {
            if (was_connected) kvm_transport_core_reset(&core);
            was_connected = false;
            publish_status(false, guest_ready);
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
        if ((button_bits & 1u) && !(button_bits & 2u))
            (void)kvm_transport_core_device_select_request(&core, router.slot ? 0 : 1);
        publish_status(true, guest_ready);
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
    if (xTaskCreate(usb_worker, "kvm_usb_loopback", KVM_USB_TASK_STACK, NULL, 10, &usb_task) != pdPASS) {
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
