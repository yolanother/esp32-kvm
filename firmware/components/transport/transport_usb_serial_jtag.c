/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Bridges binary framed routing traffic through ESP32-S3 USB Serial/JTAG CDC.
 * One worker serializes transport and router calls, rotates the session on
 * disconnect, ticks the fail-local lease, and queues NimBLE pairing events
 * for serialized minor-one STATUS. It refreshes only the authenticated
 * connected peer's opaque token through a bounded HID host-loop RPC. */
#include "transport_usb_serial_jtag.h"
#include "transport_core.h"
#include "router_hid_bridge.h"
#include "display.h"
#include "hid_guest.h"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdatomic.h>
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
static QueueHandle_t pairing_queue;
static atomic_bool pairing_overflow;

static bool pair_begin(void *context) { (void)context; return hid_guest_request_pair_begin(); }
static bool pair_cancel(void *context) { (void)context; return hid_guest_request_pair_cancel(); }
static bool pair_reply(void *context, uint32_t id, bool approved)
{ (void)context; return hid_guest_request_pair_reply(id, approved); }
static bool pair_forget(void *context, const uint8_t token[16])
{ (void)context; return hid_guest_request_forget_bond(token); }

static void pairing_event(const hid_guest_pairing_event_t *event, void *context)
{
    (void)context;
    if (!pairing_queue || xQueueSend(pairing_queue, event, 0) != pdTRUE)
        atomic_store(&pairing_overflow, true);
}

static void drain_pairing_events(uint8_t connected_token[16])
{
    hid_guest_pairing_event_t event;
    if (atomic_exchange(&pairing_overflow, false)) {
        (void)xQueueReset(pairing_queue);
        memset(connected_token, 0, 16);
        kvm_transport_core_set_connected_token(&core, NULL);
        (void)hid_guest_request_pair_cancel();
        kvm_transport_core_pairing_event(&core, KVM_PAIRING_REJECTED, 0, 0, 0);
        return;
    }
    while (xQueueReceive(pairing_queue, &event, 0) == pdTRUE) {
        if (event.type == HID_GUEST_DISCONNECTED) {
            memset(connected_token, 0, 16);
            kvm_transport_core_set_connected_token(&core, NULL);
            continue;
        }
        kvm_transport_pairing_state_t state;
        switch (event.type) {
        case HID_GUEST_PAIRING_OPENED: state = KVM_PAIRING_WAITING; break;
        case HID_GUEST_PAIRING_CHALLENGE: state = KVM_PAIRING_CHALLENGE; break;
        case HID_GUEST_PAIRING_REJECTED: state = KVM_PAIRING_REJECTED; break;
        case HID_GUEST_PAIRING_CAPACITY: state = KVM_PAIRING_CAPACITY; break;
        case HID_GUEST_PAIRING_TIMEOUT: state = KVM_PAIRING_TIMEOUT; break;
        default: state = KVM_PAIRING_CLOSED; break;
        }
        kvm_transport_core_pairing_event(&core, state, event.challenge_id,
                                         event.number, event.deadline_ms);
    }
}

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
    bool ready_sampled = false;
    uint8_t connected_token[16] = {0};
    for (;;) {
        uint32_t button_bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &button_bits, 0);
        if (button_bits & 2u) {
            (void)hid_guest_request_pair_cancel();
            kvm_router_emergency_release(&router);
            kvm_transport_core_reset(&core);
            was_connected = false;
        }
        bool connected = usb_serial_jtag_is_connected();
        uint64_t time_ms = now_ms(NULL);
        drain_pairing_events(connected_token);
        if (!ready_sampled || time_ms < last_ready_ms || time_ms - last_ready_ms >= 1000) {
            guest_ready = hid_guest_request_ready();
            memset(connected_token, 0, sizeof(connected_token));
            (void)hid_guest_request_current_bond_token(connected_token);
            if (was_connected)
                kvm_transport_core_set_connected_token(&core, connected_token);
            last_ready_ms = time_ms;
            ready_sampled = true;
        }
        if (!connected) {
            if (was_connected) {
                (void)hid_guest_request_pair_cancel();
                kvm_transport_core_reset(&core);
            }
            was_connected = false;
            publish_status(false, guest_ready);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!was_connected) {
            (void)kvm_transport_core_init(&core, "esp32-kvm-s3", "0.1.0-m1",
                                          new_session(), send_binary, NULL);
            kvm_transport_core_bind_router(&core, &router, now_ms, NULL);
            kvm_transport_core_bind_pairing(&core,
                (kvm_transport_pairing_ops_t){pair_begin, pair_cancel, pair_reply, pair_forget}, NULL);
            kvm_transport_core_set_connected_token(&core, connected_token);
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
    pairing_queue = xQueueCreate(8, sizeof(hid_guest_pairing_event_t));
    if (!pairing_queue) {
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    hid_guest_pairing_set_events(pairing_event, NULL);
    kvm_router_init(&router, kvm_router_hid_output(), NULL);
    if (xTaskCreate(usb_worker, "kvm_usb_loopback", KVM_USB_TASK_STACK, NULL, 10, &usb_task) != pdPASS) {
        hid_guest_pairing_set_events(NULL, NULL);
        vQueueDelete(pairing_queue);
        pairing_queue = NULL;
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
