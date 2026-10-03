/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Bridges binary framed routing traffic through ESP32-S3 USB Serial/JTAG CDC.
 * One worker serializes transport and router calls, rotates the session on
 * disconnect, ticks the fail-local lease, and queues NimBLE pairing events
 * for serialized minor-one STATUS. It refreshes three authenticated
 * connected peer slots through one bounded HID host-loop RPC. A
 * separate on-demand RPC supplies retained tokens for minor-two inventory.
 * The same serialized facts feed a fail-closed device screen projection. */
#include "transport_usb_serial_jtag.h"
#include "transport_core.h"
#include "router_hid_bridge.h"
#include "transport_display_status.h"
#include "display.h"
#include "display_pairing.h"
#include "hid_guest.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
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
static QueueHandle_t pairing_touch_queue;
static atomic_bool pairing_overflow;
static kvm_display_pairing_t display_pairing;

static bool pair_begin(void *context)
{
    (void)context;
    if (display_pairing.state == KVM_DISPLAY_PAIRING_WAITING ||
        display_pairing.state == KVM_DISPLAY_PAIRING_CHALLENGE) return false;
    if (!hid_guest_request_pair_begin()) return false;
    kvm_display_pairing_started(&display_pairing, false, (uint64_t)esp_timer_get_time() / 1000);
    return true;
}
static bool pair_cancel(void *context)
{
    (void)context;
    if (display_pairing.local_owner || !hid_guest_request_pair_cancel()) return false;
    kvm_display_pairing_clear(&display_pairing);
    return true;
}
static bool pair_reply(void *context, uint32_t id, bool approved)
{
    (void)context;
    if (display_pairing.local_owner || !hid_guest_request_pair_reply(id, approved)) return false;
    kvm_display_pairing_replied(&display_pairing, approved);
    return true;
}
static bool pair_forget(void *context, const uint8_t token[16])
{ (void)context; return hid_guest_request_forget_bond(token); }
static bool pair_inventory(void *context, uint8_t tokens[8][16], uint8_t *count)
{ (void)context; return hid_guest_request_retained_bonds(tokens, count); }

static void pairing_event(const hid_guest_pairing_event_t *event, void *context)
{
    (void)context;
    if (!pairing_queue || xQueueSend(pairing_queue, event, 0) != pdTRUE)
        atomic_store(&pairing_overflow, true);
}

static bool drain_pairing_events(void)
{
    hid_guest_pairing_event_t event;
    bool disconnected = false;
    if (atomic_exchange(&pairing_overflow, false)) {
        (void)xQueueReset(pairing_queue);
        disconnected = true;
        if (router.slot || router.armed) kvm_router_emergency_release(&router);
        (void)hid_guest_request_pair_cancel();
        kvm_display_pairing_clear(&display_pairing);
        kvm_transport_core_pairing_event(&core, KVM_PAIRING_REJECTED, 0, 0, 0);
        return disconnected;
    }
    while (xQueueReceive(pairing_queue, &event, 0) == pdTRUE) {
        if (event.type == HID_GUEST_DISCONNECTED) {
            disconnected = true;
            if (router.slot && (!event.number || event.number == router.slot))
                kvm_router_emergency_release(&router);
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
        if (event.type == HID_GUEST_PAIRING_OPENED) {
            if (display_pairing.state == KVM_DISPLAY_PAIRING_CLOSED)
                kvm_display_pairing_started(&display_pairing, false,
                                            (uint64_t)esp_timer_get_time() / 1000);
        } else {
            (void)kvm_display_pairing_event(&display_pairing,
                (kvm_display_pairing_state_t)state, event.challenge_id,
                event.number, event.deadline_ms,
                (uint64_t)esp_timer_get_time() / 1000);
        }
        kvm_transport_core_pairing_event(&core, state, event.challenge_id,
                                         event.number, event.deadline_ms);
    }
    return disconnected;
}

void kvm_transport_pairing_touch(kvm_display_pair_request_t request)
{
    if (pairing_touch_queue) (void)xQueueSend(pairing_touch_queue, &request, 0);
}

static void drain_pairing_touch(uint64_t time_ms)
{
    kvm_display_pair_request_t request;
    while (xQueueReceive(pairing_touch_queue, &request, 0) == pdTRUE) {
        if (!kvm_display_pairing_accept(&display_pairing, request, time_ms)) continue;
        switch (request.action) {
        case KVM_DISPLAY_PAIR_BEGIN:
            if (hid_guest_request_pair_begin())
                kvm_display_pairing_started(&display_pairing, true, time_ms);
            break;
        case KVM_DISPLAY_PAIR_CANCEL:
            if (hid_guest_request_pair_cancel()) kvm_display_pairing_clear(&display_pairing);
            break;
        case KVM_DISPLAY_PAIR_APPROVE:
        case KVM_DISPLAY_PAIR_REJECT:
            if (hid_guest_request_pair_reply(request.challenge_id,
                    request.action == KVM_DISPLAY_PAIR_APPROVE))
                kvm_display_pairing_replied(&display_pairing,
                    request.action == KVM_DISPLAY_PAIR_APPROVE);
            break;
        default: break;
        }
    }
}

void kvm_transport_button_event(kvm_display_event_t event)
{
    if (!usb_task) return;
    uint32_t bit = event == KVM_DISPLAY_EMERGENCY_RELEASE ? 2u :
                   event == KVM_DISPLAY_NEXT_REQUEST ? 1u : 0u;
    if (bit) (void)xTaskNotify(usb_task, bit, eSetBits);
}

static void publish_status(bool connected, bool guest_ready,
                           const kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS])
{
    kvm_display_status_t status;
    kvm_transport_display_status(&core, &router, connected, guest_ready,
                                 (uint64_t)(esp_timer_get_time() / 1000), &status);
    /* USB can remain electrically connected after the verified host session
       closes. Keep the authenticated BLE slot visible in either case. */
    if (!connected || !core.session_open || !router.session_open) {
        for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
            bool has_token = false;
            for (size_t i = 0; i < sizeof(slots[slot].token); ++i)
                has_token |= slots[slot].token[i] != 0;
            if (!has_token) continue;
            status.guest_slots = slot + 1u;
            if (slots[slot].ready && slots[slot].subscribed) {
                status.ready_slots++;
                status.ready_mask |= (uint8_t)(1u << slot);
            }
        }
    }
    if (display_pairing.state != KVM_DISPLAY_PAIRING_CLOSED) {
        status.pairing_state = display_pairing.state;
        status.pairing_local_owner = display_pairing.local_owner;
        status.pairing_challenge_id = display_pairing.challenge_id;
        status.pairing_number = display_pairing.number;
        status.pairing_deadline_ms = display_pairing.deadline_ms;
    }
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
    kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS] = {0};
    for (;;) {
        uint32_t button_bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &button_bits, 0);
        if (button_bits & 2u) {
            (void)hid_guest_request_pair_cancel();
            (void)xQueueReset(pairing_touch_queue);
            kvm_display_pairing_clear(&display_pairing);
            kvm_router_emergency_release(&router);
            kvm_transport_core_reset(&core);
            was_connected = false;
        }
        bool connected = usb_serial_jtag_is_connected();
        uint64_t time_ms = now_ms(NULL);
        if (drain_pairing_events()) ready_sampled = false;
        kvm_display_pairing_expire(&display_pairing, time_ms);
        if (!(button_bits & 2u)) drain_pairing_touch(time_ms);
        if (!ready_sampled || time_ms < last_ready_ms || time_ms - last_ready_ms >= 1000) {
            hid_guest_slot_snapshot_t sampled[HID_GATT_MAX_CONNECTIONS];
            bool valid = hid_guest_request_slots(sampled);
            memset(slots, 0, sizeof(slots));
            if (valid) for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
                memcpy(slots[slot].token, sampled[slot].token, 16);
                slots[slot].ready = sampled[slot].ready;
                slots[slot].subscribed = sampled[slot].subscribed;
            }
            memset(sampled, 0, sizeof(sampled));
            guest_ready = false;
            for (uint8_t slot = 0; valid && slot < KVM_ROUTER_MAX_SLOTS; ++slot)
                guest_ready |= slots[slot].ready && slots[slot].subscribed;
            if (was_connected && (!valid || !kvm_transport_core_set_slots(&core, slots))) {
                memset(slots, 0, sizeof(slots));
                (void)kvm_transport_core_set_slots(&core, slots);
                if (router.slot || router.armed) kvm_router_emergency_release(&router);
            }
            last_ready_ms = time_ms;
            ready_sampled = true;
        }
        if (!connected) {
            if (was_connected) {
                if (!display_pairing.local_owner) {
                    (void)hid_guest_request_pair_cancel();
                    kvm_display_pairing_clear(&display_pairing);
                }
                kvm_transport_core_reset(&core);
            }
            was_connected = false;
            publish_status(false, guest_ready, slots);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!was_connected) {
            (void)kvm_transport_core_init(&core, "esp32-kvm-s3", esp_app_get_description()->version,
                                          new_session(), send_binary, NULL);
            kvm_transport_core_bind_router(&core, &router, now_ms, NULL);
            kvm_transport_core_bind_pairing(&core,
                (kvm_transport_pairing_ops_t){pair_begin, pair_cancel, pair_reply,
                                               pair_forget, pair_inventory}, NULL);
            if (!kvm_transport_core_set_slots(&core, slots)) {
                memset(slots, 0, sizeof(slots));
                (void)kvm_transport_core_set_slots(&core, slots);
            }
            was_connected = true;
        }
        int read = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        if (read > 0) kvm_transport_core_feed(&core, bytes, (size_t)read);
        kvm_transport_core_tick(&core);
        if ((button_bits & 1u) && !(button_bits & 2u)) {
            uint8_t ready_mask = 0;
            for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot)
                if (slots[slot].ready && slots[slot].subscribed)
                    ready_mask |= (uint8_t)(1u << slot);
            (void)kvm_transport_core_device_select_request(&core,
                kvm_display_next_ready_slot(ready_mask, router.slot));
        }
        publish_status(true, guest_ready, slots);
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
    pairing_touch_queue = xQueueCreate(4, sizeof(kvm_display_pair_request_t));
    if (!pairing_touch_queue) {
        vQueueDelete(pairing_queue);
        pairing_queue = NULL;
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    hid_guest_pairing_set_events(pairing_event, NULL);
    kvm_router_init(&router, kvm_router_hid_output(), NULL);
    if (!kvm_router_set_capacity(&router, KVM_ROUTER_MAX_SLOTS)) {
        hid_guest_pairing_set_events(NULL, NULL);
        vQueueDelete(pairing_queue);
        vQueueDelete(pairing_touch_queue);
        pairing_queue = NULL;
        pairing_touch_queue = NULL;
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(usb_worker, "kvm_usb_loopback", KVM_USB_TASK_STACK, NULL, 10, &usb_task) != pdPASS) {
        hid_guest_pairing_set_events(NULL, NULL);
        vQueueDelete(pairing_queue);
        vQueueDelete(pairing_touch_queue);
        pairing_queue = NULL;
        pairing_touch_queue = NULL;
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
