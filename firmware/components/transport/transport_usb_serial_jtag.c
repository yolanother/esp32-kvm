/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Bridges binary framed routing traffic through ESP32-S3 USB Serial/JTAG CDC.
 * One worker serializes transport and router calls, rotates the session on
 * disconnect, ticks the fail-local lease, and queues NimBLE pairing events
 * for serialized minor-one STATUS. It refreshes three authenticated
 * connected peer slots through one bounded HID host-loop RPC. A
 * separate on-demand RPC supplies retained tokens for minor-two inventory.
 * The same serialized facts feed a fail-closed device screen projection.
 * The input hub runs in the same worker beside the router: the BLE central
 * starts here once the NimBLE host is synced, its callbacks only copy reports
 * into a bounded queue, and the worker drains that queue each iteration after
 * the host bytes and the lease tick, bounded to 20 ms of device work.
 * The worker also owns the input-device pairing window: touch requests and
 * central pairing events update it, and it is never open beside a guest
 * pairing window (each refuses the other with BUSY) or while the host has a
 * guest armed (BEGIN refused, an open window ended); an open but unarmed host
 * session does not affect it. The same rules gate the input-device list
 * behind the entry, whose confirmed forget is the worker's only other central
 * call; the stored names it lists come from doubtech_hid_central_stored().
 * Reconnecting stored devices is the central's own schedule. */
#include "transport_usb_serial_jtag.h"
#include "transport_core.h"
#include "transport_hub.h"
#include "router_hid_bridge.h"
#include "transport_display_status.h"
#include "display.h"
#include "display_pairing.h"
#include "display_labels.h"
#include "doubtech_hid.h"
#include "doubtech_hid_central.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
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
#define KVM_USB_TASK_STACK 6144 /* worker also starts the central (NVS load) */
#define KVM_HEALTH_LOG_MS 30000u /* period of the Health log line */

static kvm_transport_core_t core;
static kvm_router_t router;
static bool started;
static TaskHandle_t usb_task;
static QueueHandle_t pairing_queue;
static QueueHandle_t pairing_touch_queue;
static QueueHandle_t target_touch_queue;
static atomic_bool pairing_overflow;
static kvm_display_pairing_t display_pairing;
/* Device input: the queue is shared with the NimBLE host thread under
 * hub_lock; hub_worker (and the router it guards) belong to the worker. */
static kvm_hub_queue_t hub_queue;
static portMUX_TYPE hub_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool hub_queue_ready;
static kvm_hub_worker_t hub_worker;
static QueueHandle_t central_event_queue;
static atomic_uint published_hub_target;
/* Input-device pairing screen: worker-owned, fed by touch and central events. */
static kvm_display_input_pairing_t input_pairing;
static QueueHandle_t input_touch_queue;

static void hub_queue_lock(void *context) { (void)context; portENTER_CRITICAL(&hub_lock); }
static void hub_queue_unlock(void *context) { (void)context; portEXIT_CRITICAL(&hub_lock); }

void kvm_transport_hub_submit(const kvm_hub_msg_t *msg)
{
    if (atomic_load(&hub_queue_ready)) (void)kvm_hub_queue_push(&hub_queue, msg);
}

uint8_t kvm_transport_hub_target(void) { return (uint8_t)atomic_load(&published_hub_target); }

/* NimBLE host thread: copy and enqueue only. */
static void central_input(const doubtech_hid_central_input_t *input, void *context)
{
    kvm_hub_msg_t msg;
    (void)context;
    if (kvm_router_hid_central_msg(input, &msg)) kvm_transport_hub_submit(&msg);
}

/* NimBLE host thread: copy and enqueue only. Pairing events feed the input
 * pairing screen; a full queue drops the event, and the screen's own hard
 * deadline still closes the window and cancels the central. */
static void central_event(const doubtech_hid_central_event_t *event, void *context)
{
    (void)context;
    if (central_event_queue) (void)xQueueSend(central_event_queue, event, 0);
}

/* Starts the BLE central from the worker once the shared NimBLE host is synced,
 * retrying a failed start with a doubling, capped delay. */
static void poll_central_start(uint64_t time_ms)
{
    static const doubtech_hid_central_config_t config = {
        .on_input = central_input, .on_event = central_event,
        .context = NULL, .store_namespace = NULL,
    };
    static bool central_started;
    static uint32_t failures;
    static uint64_t retry_at_ms;
    if (central_started || time_ms < retry_at_ms || !doubtech_hid_host_synced()) return;
    esp_err_t result = doubtech_hid_central_start(&config);
    if (result == ESP_OK) {
        central_started = true;
        return;
    }
    retry_at_ms = time_ms + kvm_hub_central_backoff_ms(failures);
    if (failures < UINT32_MAX) failures++;
    ESP_LOGW("esp32-kvm", "Input central start failed: %d", result);
}

static void drain_central_events(uint64_t time_ms)
{
    doubtech_hid_central_event_t event;
    while (central_event_queue && xQueueReceive(central_event_queue, &event, 0) == pdTRUE) {
        kvm_display_input_event_t copy;
        memset(&copy, 0, sizeof(copy));
        switch (event.type) {
        case DTHID_CENTRAL_SCAN_RESULT: copy.type = KVM_DISPLAY_INPUT_EVENT_SCAN; break;
        case DTHID_CENTRAL_PASSKEY: copy.type = KVM_DISPLAY_INPUT_EVENT_PASSKEY; break;
        case DTHID_CENTRAL_PAIRED: copy.type = KVM_DISPLAY_INPUT_EVENT_PAIRED; break;
        case DTHID_CENTRAL_PAIR_FAILED: copy.type = KVM_DISPLAY_INPUT_EVENT_FAILED; break;
        case DTHID_CENTRAL_CONNECTED:
        case DTHID_CENTRAL_DISCONNECTED:
            /* Status: publish_status reads doubtech_hid_central_connected() every
             * iteration, which changes before these events are sent, so the
             * KB/MS badge follows at once. Held input was already released
             * through the LOST report on the hub queue. */
            continue;
        default: continue;
        }
        copy.index = event.index;
        copy.kind = event.kind;
        memcpy(copy.name, event.name, sizeof(copy.name));
        copy.rssi = event.rssi;
        copy.passkey = event.passkey;
        copy.reason = event.reason;
        kvm_display_input_pairing_event(&input_pairing, &copy, time_ms);
    }
}

/* Central operations for the input pairing window; worker task only. */
/* ESP_ERR_TIMEOUT: the bounded request gave up but the host may still run it. */
static kvm_display_input_op_t central_op(esp_err_t result)
{
    return result == ESP_OK ? KVM_DISPLAY_INPUT_OP_OK :
           result == ESP_ERR_TIMEOUT ? KVM_DISPLAY_INPUT_OP_UNKNOWN : KVM_DISPLAY_INPUT_OP_REFUSED;
}
static kvm_display_input_op_t central_pair_begin(void *context)
{ (void)context; return central_op(doubtech_hid_central_pair_begin()); }
static kvm_display_input_op_t central_pair_select(void *context, uint8_t index)
{ (void)context; return central_op(doubtech_hid_central_pair_select(index)); }
static bool central_pair_cancel(void *context)
{ (void)context; return doubtech_hid_central_pair_cancel() == ESP_OK; }
/* The central saves the registry, drops the link (lost input, then
 * DISCONNECTED) and stops that kind's reconnects. */
static kvm_display_input_op_t central_forget(void *context, uint8_t kind)
{ (void)context; return central_op(doubtech_hid_central_forget(kind)); }

/* The stored devices for the device list, read from the central's published
 * copy (cheap; any task) before touches are applied and the status is built. */
static void refresh_input_devices(void)
{
    for (uint8_t kind = DOUBTECH_HID_CENTRAL_KEYBOARD; kind <= DOUBTECH_HID_CENTRAL_MOUSE; ++kind) {
        char name[DOUBTECH_HID_CENTRAL_NAME_BYTES];
        bool stored = doubtech_hid_central_stored(kind, name);
        kvm_display_input_pairing_device(&input_pairing, kind, stored, name);
    }
}

/* A guest window the worker did not open, cancelled for the input window;
 * the cancel RPC is retried (once a second) until it succeeds, the guest
 * window reports its end, or the guest window's own deadline passes. */
static bool guest_cancel_pending;
static uint64_t guest_cancel_until_ms;
static uint64_t guest_cancel_retry_ms;

static void cancel_foreign_guest_window(uint64_t time_ms, uint64_t window_deadline_ms)
{
    if (doubtech_hid_request_pair_cancel()) {
        guest_cancel_pending = false;
        return;
    }
    if (!guest_cancel_pending) {
        guest_cancel_pending = true;
        guest_cancel_until_ms = window_deadline_ms > time_ms ? window_deadline_ms : time_ms + 60000u;
    }
    guest_cancel_retry_ms = time_ms + KVM_DISPLAY_INPUT_CANCEL_RETRY_MS;
}

static void poll_foreign_guest_cancel(uint64_t time_ms)
{
    if (!guest_cancel_pending) return;
    if (time_ms >= guest_cancel_until_ms) guest_cancel_pending = false;
    else if (time_ms >= guest_cancel_retry_ms) cancel_foreign_guest_window(time_ms, guest_cancel_until_ms);
}

/* The guest window as the worker knows it (OPENED events and its own begins).
 * doubtech_hid_pairing_active() itself is checked by the central on the
 * NimBLE host thread inside pair_begin; calling it here would race the host. */
static bool guest_pairing_open(void)
{
    return display_pairing.state == KVM_DISPLAY_PAIRING_WAITING ||
           display_pairing.state == KVM_DISPLAY_PAIRING_CHALLENGE;
}

void kvm_transport_input_touch(kvm_display_input_request_t request)
{
    if (input_touch_queue) (void)xQueueSend(input_touch_queue, &request, 0);
}

static void drain_input_touch(uint64_t time_ms)
{
    kvm_display_input_request_t request;
    while (input_touch_queue && xQueueReceive(input_touch_queue, &request, 0) == pdTRUE)
        (void)kvm_display_input_pairing_request(&input_pairing, request, time_ms,
                                                router.armed, guest_pairing_open());
}

static uint8_t visible_ready_mask(const kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS])
{
    uint8_t ready_mask = 0;
    for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot)
        if (slots[slot].ready && slots[slot].subscribed &&
            !kvm_display_guest_hidden(slots[slot].token))
            ready_mask |= (uint8_t)(1u << slot);
    return ready_mask;
}

static bool pair_begin(void *context)
{
    (void)context;
    if (display_pairing.state == KVM_DISPLAY_PAIRING_WAITING ||
        display_pairing.state == KVM_DISPLAY_PAIRING_CHALLENGE) return false;
    /* Never beside an input-device window (the app gets BUSY, the screen too). */
    if (kvm_display_input_pairing_blocks_guest(&input_pairing)) {
        kvm_display_input_pairing_guest_refused(&input_pairing, (uint64_t)esp_timer_get_time() / 1000);
        return false;
    }
    if (!doubtech_hid_request_pair_begin()) return false;
    kvm_display_pairing_started(&display_pairing, false, (uint64_t)esp_timer_get_time() / 1000);
    return true;
}
static bool pair_cancel(void *context)
{
    (void)context;
    if (display_pairing.local_owner || !doubtech_hid_request_pair_cancel()) return false;
    kvm_display_pairing_clear(&display_pairing);
    return true;
}
static bool pair_reply(void *context, uint32_t id, bool approved)
{
    (void)context;
    if (display_pairing.local_owner || !doubtech_hid_request_pair_reply(id, approved)) return false;
    kvm_display_pairing_replied(&display_pairing, approved);
    return true;
}
static bool pair_forget(void *context, const uint8_t token[16])
{ (void)context; return doubtech_hid_request_forget_bond(token); }
static bool pair_inventory(void *context, uint8_t tokens[8][16], uint8_t *count)
{ (void)context; return doubtech_hid_request_retained_bonds(tokens, count); }
static bool set_label(void *context, const uint8_t token[16], const char *name, size_t length)
{ (void)context; return kvm_display_label_set(token, name, length); }
static bool set_enabled(void *context, const uint8_t token[16], bool enabled)
{ (void)context; return kvm_display_guest_hidden_set(token, !enabled); }

static void pairing_event(const doubtech_hid_pairing_event_t *event, void *context)
{
    (void)context;
    if (!pairing_queue || xQueueSend(pairing_queue, event, 0) != pdTRUE)
        atomic_store(&pairing_overflow, true);
}

static bool drain_pairing_events(void)
{
    doubtech_hid_pairing_event_t event;
    bool disconnected = false;
    if (atomic_exchange(&pairing_overflow, false)) {
        (void)xQueueReset(pairing_queue);
        disconnected = true;
        if (router.slot || router.armed) kvm_router_emergency_release(&router);
        kvm_hub_worker_release(&hub_worker); /* a missed disconnect may hide behind the overflow */
        (void)doubtech_hid_request_pair_cancel();
        kvm_display_pairing_clear(&display_pairing);
        kvm_transport_core_pairing_event(&core, KVM_PAIRING_REJECTED, 0, 0, 0);
        return disconnected;
    }
    while (xQueueReceive(pairing_queue, &event, 0) == pdTRUE) {
        if (event.type == DOUBTECH_HID_DISCONNECTED) {
            disconnected = true;
            if (router.slot && (!event.number || event.number == router.slot))
                kvm_router_emergency_release(&router);
            kvm_hub_worker_slot_lost(&hub_worker, event.number);
            continue;
        }
        kvm_transport_pairing_state_t state;
        switch (event.type) {
        case DOUBTECH_HID_PAIRING_OPENED: state = KVM_PAIRING_WAITING; break;
        case DOUBTECH_HID_PAIRING_CHALLENGE: state = KVM_PAIRING_CHALLENGE; break;
        case DOUBTECH_HID_PAIRING_REJECTED: state = KVM_PAIRING_REJECTED; break;
        case DOUBTECH_HID_PAIRING_CAPACITY: state = KVM_PAIRING_CAPACITY; break;
        case DOUBTECH_HID_PAIRING_TIMEOUT: state = KVM_PAIRING_TIMEOUT; break;
        default: state = KVM_PAIRING_CLOSED; break;
        }
        if (event.type == DOUBTECH_HID_PAIRING_OPENED &&
            kvm_display_input_pairing_blocks_guest(&input_pairing)) {
            /* A guest window the worker did not open (a guest starting
               pairing on its own) never runs beside the input window; its
               CLOSED event follows the cancel, which is retried on failure. */
            uint64_t time_ms = (uint64_t)esp_timer_get_time() / 1000;
            cancel_foreign_guest_window(time_ms, event.deadline_ms);
            kvm_display_input_pairing_guest_refused(&input_pairing, time_ms);
        } else if (event.type == DOUBTECH_HID_PAIRING_OPENED) {
            if (display_pairing.state == KVM_DISPLAY_PAIRING_CLOSED)
                kvm_display_pairing_started(&display_pairing, false,
                                            (uint64_t)esp_timer_get_time() / 1000);
        } else {
            if (event.type != DOUBTECH_HID_PAIRING_CHALLENGE)
                guest_cancel_pending = false; /* that guest window has ended */
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

void kvm_transport_target_touch(uint8_t slot)
{
    if (target_touch_queue && slot <= KVM_ROUTER_MAX_SLOTS)
        (void)xQueueSend(target_touch_queue, &slot, 0);
}

static void drain_pairing_touch(uint64_t time_ms)
{
    kvm_display_pair_request_t request;
    while (xQueueReceive(pairing_touch_queue, &request, 0) == pdTRUE) {
        if (!kvm_display_pairing_accept(&display_pairing, request, time_ms)) continue;
        if (request.action == KVM_DISPLAY_PAIR_BEGIN &&
            kvm_display_input_pairing_blocks_guest(&input_pairing)) {
            kvm_display_input_pairing_guest_refused(&input_pairing, time_ms); /* shows BUSY */
            continue;
        }
        switch (request.action) {
        case KVM_DISPLAY_PAIR_BEGIN:
            if (doubtech_hid_request_pair_begin())
                kvm_display_pairing_started(&display_pairing, true, time_ms);
            break;
        case KVM_DISPLAY_PAIR_CANCEL:
            if (doubtech_hid_request_pair_cancel()) kvm_display_pairing_clear(&display_pairing);
            break;
        case KVM_DISPLAY_PAIR_APPROVE:
        case KVM_DISPLAY_PAIR_REJECT:
            if (doubtech_hid_request_pair_reply(request.challenge_id,
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

static void publish_status(bool connected, bool guest_ready, uint8_t paired_guests,
                           const kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS])
{
    kvm_display_status_t status;
    kvm_transport_display_status(&core, &router, connected, guest_ready,
                                 (uint64_t)(esp_timer_get_time() / 1000), &status);
    status.paired_guests = paired_guests;
    status.host_session = router.session_open;
    status.input_kbd = doubtech_hid_central_connected(DOUBTECH_HID_CENTRAL_KEYBOARD);
    status.input_mouse = doubtech_hid_central_connected(DOUBTECH_HID_CENTRAL_MOUSE);
    status.hub_target = hub_worker.hub.target;
    kvm_display_input_pairing_status(&input_pairing, (uint64_t)(esp_timer_get_time() / 1000), &status);
    /* USB can remain electrically connected after the verified host session
       closes. Keep the authenticated BLE slot visible in either case. */
    if (!connected || !core.session_open || !router.session_open) {
        for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
            bool has_token = false;
            for (size_t i = 0; i < sizeof(slots[slot].token); ++i)
                has_token |= slots[slot].token[i] != 0;
            if (!has_token) continue;
            status.occupied_mask |= (uint8_t)(1u << slot);
            status.guest_slots = slot + 1u;
            if (slots[slot].ready && slots[slot].subscribed) {
                status.ready_slots++;
                status.ready_mask |= (uint8_t)(1u << slot);
            }
        }
    }
    /* A guest the host disabled stays bonded and connected but leaves the screen. */
    for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
        uint8_t bit = (uint8_t)(1u << slot);
        if (!(status.occupied_mask & bit) || !kvm_display_guest_hidden(slots[slot].token)) continue;
        status.occupied_mask &= (uint8_t)~bit;
        if (status.ready_mask & bit) {
            status.ready_mask &= (uint8_t)~bit;
            if (status.ready_slots) status.ready_slots--;
        }
    }
    status.guest_slots = 0;
    for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot)
        if (status.occupied_mask & (1u << slot)) status.guest_slots = slot + 1u;
    for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
        bool has_token = false;
        for (size_t i = 0; i < sizeof(slots[slot].token); ++i)
            has_token |= slots[slot].token[i] != 0;
        if (!has_token) continue;
        (void)kvm_display_label_get(slots[slot].token, status.guest_names[slot]);
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
    uint8_t paired_guests = 0;
    uint64_t last_ready_ms = 0;
    uint64_t last_health_ms = 0;
    bool ready_sampled = false;
    kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS] = {0};
    for (;;) {
        uint32_t button_bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &button_bits, 0);
        if (button_bits & 2u) {
            (void)doubtech_hid_request_pair_cancel();
            (void)xQueueReset(pairing_touch_queue);
            kvm_display_pairing_clear(&display_pairing);
            (void)xQueueReset(input_touch_queue);
            (void)kvm_display_input_pairing_request(&input_pairing,
                (kvm_display_input_request_t){.action = KVM_DISPLAY_INPUT_PAIR_CANCEL,
                                              .window = input_pairing.window},
                now_ms(NULL), router.armed, guest_pairing_open());
            kvm_hub_worker_release(&hub_worker);
            kvm_router_emergency_release(&router);
            kvm_transport_core_reset(&core);
            was_connected = false;
        }
        bool connected = usb_serial_jtag_is_connected();
        uint64_t time_ms = now_ms(NULL);
        if (time_ms - last_health_ms >= KVM_HEALTH_LOG_MS || time_ms < last_health_ms) {
            last_health_ms = time_ms;
            /* Owner checklist: free heap and the smallest free stack seen by the two busiest tasks. */
            ESP_LOGI("esp32-kvm", "Health: free=%u min_free=%u internal_min_free=%u usb_worker_stack_min=%u display_stack_min=%u",
                     (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                     (unsigned)kvm_display_stack_high_water_bytes());
        }
        if (drain_pairing_events()) ready_sampled = false;
        kvm_display_pairing_expire(&display_pairing, time_ms);
        if (!(button_bits & 2u)) drain_pairing_touch(time_ms);
        uint8_t touched_slot;
        /* With no host session a card tap selects the device-input target;
         * with one it stays a request to the host, as before. */
        while (target_touch_queue && xQueueReceive(target_touch_queue, &touched_slot, 0) == pdTRUE)
            if (!kvm_hub_worker_select(&hub_worker, touched_slot))
                (void)kvm_transport_core_device_select_request(&core, touched_slot);
        if (!ready_sampled || time_ms < last_ready_ms || time_ms - last_ready_ms >= 1000) {
            doubtech_hid_slot_t sampled[DOUBTECH_HID_MAX_CONNECTIONS];
            bool valid = doubtech_hid_request_slots(sampled);
            uint8_t retained[DOUBTECH_HID_MAX_BONDS][DOUBTECH_HID_TOKEN_LEN] = {{0}};
            uint8_t retained_count = 0;
            if (doubtech_hid_request_retained_bonds(retained, &retained_count))
                paired_guests = retained_count;
            memset(retained, 0, sizeof(retained));
            memset(slots, 0, sizeof(slots));
            if (valid) for (uint8_t slot = 0; slot < KVM_ROUTER_MAX_SLOTS; ++slot) {
                memcpy(slots[slot].token, sampled[slot].token, 16);
                slots[slot].ready = sampled[slot].ready;
                slots[slot].subscribed = sampled[slot].subscribed;
                slots[slot].notify_tx_successes = sampled[slot].notify_tx_successes;
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
        bool plus = (button_bits & 1u) && !(button_bits & 2u);
        if (plus && !router.session_open) {
            /* PLUS steps the device-input target while no host session owns it. */
            (void)kvm_hub_worker_select(&hub_worker,
                kvm_display_next_ready_slot(visible_ready_mask(slots), hub_worker.hub.target));
            plus = false;
        }
        poll_central_start(time_ms);
        refresh_input_devices();
        if (!(button_bits & 2u)) drain_input_touch(time_ms);
        drain_central_events(time_ms);
        /* Hard deadline, unconfirmed-cancel retries and the host-armed rule,
           even without central events. An open, unarmed session (the PC app
           just connected) leaves the window alone; the router arming ends it. */
        kvm_display_input_pairing_tick(&input_pairing, time_ms, router.armed);
        poll_foreign_guest_cancel(time_ms);
        /* Device input waiting: poll USB briefly so it is not held back 20 ms. */
        TickType_t wait = pdMS_TO_TICKS(kvm_hub_queue_pending(&hub_queue) ? 2 : 20);
        if (!wait) wait = 1;
        if (!connected) {
            if (was_connected) {
                if (!display_pairing.local_owner) {
                    (void)doubtech_hid_request_pair_cancel();
                    kvm_display_pairing_clear(&display_pairing);
                }
                kvm_transport_core_reset(&core);
            }
            was_connected = false;
            kvm_hub_worker_iteration(&hub_worker, &hub_queue, NULL, NULL, 0);
            atomic_store(&published_hub_target, hub_worker.hub.target);
            publish_status(false, guest_ready, paired_guests, slots);
            vTaskDelay(wait);
            continue;
        }
        if (!was_connected) {
            (void)kvm_transport_core_init(&core, "esp32-kvm-s3", esp_app_get_description()->version,
                                          new_session(), send_binary, NULL);
            kvm_transport_core_bind_router(&core, &router, now_ms, NULL);
            kvm_transport_core_bind_pairing(&core,
                (kvm_transport_pairing_ops_t){
                    .begin = pair_begin, .cancel = pair_cancel, .reply = pair_reply,
                    .forget = pair_forget, .inventory = pair_inventory, .label = set_label,
                    .enabled = set_enabled,
                }, NULL);
            if (!kvm_transport_core_set_slots(&core, slots)) {
                memset(slots, 0, sizeof(slots));
                (void)kvm_transport_core_set_slots(&core, slots);
            }
            was_connected = true;
        }
        int read = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), wait);
        size_t host_bytes = 0;
        if (read > 0) {
            if (kvm_transport_diagnostics_requested(bytes, (size_t)read)) {
                kvm_router_emergency_release(&router);
                kvm_transport_core_reset(&core);
                kvm_transport_diagnostics_dump();
                was_connected = false;
            } else host_bytes = (size_t)read;
        }
        /* Host bytes (heartbeats, ARM) and the router tick (lease) are served
         * first; only then the time-bounded device drain runs. */
        kvm_hub_worker_iteration(&hub_worker, &hub_queue, &core, bytes, host_bytes);
        atomic_store(&published_hub_target, hub_worker.hub.target);
        if (plus) {
            (void)kvm_transport_core_device_select_request(&core,
                kvm_display_next_ready_slot(visible_ready_mask(slots), router.slot));
        }
        publish_status(true, guest_ready, paired_guests, slots);
    }
}

static void delete_queues(void)
{
    QueueHandle_t *queues[] = {&pairing_queue, &pairing_touch_queue, &target_touch_queue,
                               &central_event_queue, &input_touch_queue};
    for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i) {
        if (*queues[i]) vQueueDelete(*queues[i]);
        *queues[i] = NULL;
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
    pairing_queue = xQueueCreate(8, sizeof(doubtech_hid_pairing_event_t));
    pairing_touch_queue = xQueueCreate(4, sizeof(kvm_display_pair_request_t));
    target_touch_queue = xQueueCreate(4, sizeof(uint8_t));
    /* Holds a full scan list (8 rows) plus the window's terminal events. */
    central_event_queue = xQueueCreate(16, sizeof(doubtech_hid_central_event_t));
    input_touch_queue = xQueueCreate(4, sizeof(kvm_display_input_request_t));
    kvm_display_input_pairing_init(&input_pairing, (kvm_display_input_ops_t){
        central_pair_begin, central_pair_select, central_pair_cancel, NULL, central_forget});
    if (!pairing_queue || !pairing_touch_queue || !target_touch_queue || !central_event_queue ||
        !input_touch_queue) {
        delete_queues();
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    doubtech_hid_pairing_set_events(pairing_event, NULL);
    /* The hub and the router share the HID output; the router reaches it
     * through the hub worker so its arm and release keep the hub consistent. */
    kvm_hub_worker_init(&hub_worker, &router, kvm_router_hid_output(), NULL);
    kvm_hub_worker_set_clock(&hub_worker, now_ms, NULL);
    kvm_router_init(&router, kvm_hub_worker_router_output(), &hub_worker);
    kvm_hub_queue_init(&hub_queue, hub_queue_lock, hub_queue_unlock, NULL);
    if (!kvm_router_set_capacity(&router, KVM_ROUTER_MAX_SLOTS)) {
        doubtech_hid_pairing_set_events(NULL, NULL);
        delete_queues();
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&hub_queue_ready, true);
    if (xTaskCreate(usb_worker, "kvm_usb_loopback", KVM_USB_TASK_STACK, NULL, 10, &usb_task) != pdPASS) {
        atomic_store(&hub_queue_ready, false);
        doubtech_hid_pairing_set_events(NULL, NULL);
        delete_queues();
        (void)usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
