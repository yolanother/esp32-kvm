/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Serializes bounded HID ready, arm, release, and report requests from the
 * USB worker onto NimBLE's host loop. A timed-out request fails closed and
 * queues a disconnect; pairing commands use the same bounded host-loop bridge
 * and no HID or pairing state is read on the USB worker. */
#include "hid_guest.h"
#include "hid_guest_rpc.h"
#include <string.h>
#include <stdatomic.h>
#include "nimble/nimble_port.h"
#include "hid_gatt.h"

#define RPC_TIMEOUT_MS 20u
typedef enum { RPC_READY, RPC_ARM, RPC_RELEASE, RPC_KEYBOARD, RPC_MOUSE, RPC_CONSUMER,
               RPC_PAIR_BEGIN, RPC_PAIR_CANCEL, RPC_PAIR_REPLY } rpc_op_t;
typedef struct { uint8_t buttons; int16_t dx, dy; int8_t wheel, pan; } mouse_args_t;
typedef struct { uint32_t challenge_id; bool approved; } pair_reply_args_t;
typedef struct {
    struct ble_npl_event event;
    struct ble_npl_mutex mutex;
    struct ble_npl_sem completion;
    bool initialized;
    bool busy;
    atomic_bool cancelled;
    bool finished;
    bool result;
    rpc_op_t operation;
    ble_npl_time_t deadline;
    uint8_t keyboard[HID_KEYBOARD_REPORT_LEN];
    uint8_t buttons;
    int16_t dx, dy;
    int8_t wheel, pan;
    uint16_t usage;
    pair_reply_args_t pair_reply;
} rpc_state_t;
static rpc_state_t rpc;

static bool channel_ready(const hid_channel_t *channel)
{
    return channel->connected && channel->encrypted && !channel->needs_disconnect &&
           channel->subscribed[HID_REPORT_KEYBOARD] &&
           channel->subscribed[HID_REPORT_MOUSE] &&
           channel->subscribed[HID_REPORT_CONSUMER];
}

static void on_host(struct ble_npl_event *event)
{
    (void)event;
    if (ble_npl_mutex_pend(&rpc.mutex, 0) != BLE_NPL_OK) return;
    if (!rpc.busy) { ble_npl_mutex_release(&rpc.mutex); return; }
    bool execute = !atomic_load(&rpc.cancelled) &&
                   (int32_t)(rpc.deadline - ble_npl_time_get()) > 0;
    rpc.result = false;
    if (execute) {
        hid_channel_t *channel = hid_gatt_channel();
        switch (rpc.operation) {
        case RPC_READY: rpc.result = channel_ready(channel); break;
        case RPC_ARM: rpc.result = hid_channel_arm(channel); break;
        case RPC_RELEASE: rpc.result = hid_channel_release(channel); break;
        case RPC_KEYBOARD: rpc.result = hid_channel_keyboard(channel, rpc.keyboard); break;
        case RPC_MOUSE: rpc.result = hid_channel_mouse(channel, rpc.buttons, rpc.dx, rpc.dy,
                                                       rpc.wheel, rpc.pan); break;
        case RPC_CONSUMER: rpc.result = hid_channel_consumer(channel, rpc.usage); break;
        case RPC_PAIR_BEGIN: rpc.result = hid_guest_pairing_open() == ESP_OK; break;
        case RPC_PAIR_CANCEL: hid_guest_pairing_cancel(); rpc.result = true; break;
        case RPC_PAIR_REPLY:
            rpc.result = hid_guest_pairing_confirm(rpc.pair_reply.challenge_id,
                                                    rpc.pair_reply.approved) == ESP_OK;
            break;
        }
        if (channel->needs_disconnect && channel->connected)
            hid_guest_disconnect_current();
    }
    rpc.finished = true;
    if (atomic_load(&rpc.cancelled)) rpc.busy = false;
    ble_npl_mutex_release(&rpc.mutex);
    ble_npl_sem_release(&rpc.completion);
}

esp_err_t hid_guest_rpc_init(void)
{
    if (ble_npl_mutex_init(&rpc.mutex) != BLE_NPL_OK ||
        ble_npl_sem_init(&rpc.completion, 0) != BLE_NPL_OK) return ESP_FAIL;
    ble_npl_event_init(&rpc.event, on_host, NULL);
    rpc.initialized = true;
    return ESP_OK;
}

static bool request(rpc_op_t operation, const void *payload)
{
    if (!rpc.initialized ||
        ble_npl_mutex_pend(&rpc.mutex, 0) != BLE_NPL_OK) return false;
    if (rpc.busy) { ble_npl_mutex_release(&rpc.mutex); return false; }
    while (ble_npl_sem_pend(&rpc.completion, 0) == BLE_NPL_OK) { }
    rpc.busy = true;
    atomic_store(&rpc.cancelled, false);
    rpc.finished = false;
    rpc.result = false;
    rpc.operation = operation;
    rpc.deadline = ble_npl_time_get() + ble_npl_time_ms_to_ticks32(RPC_TIMEOUT_MS);
    if (operation == RPC_KEYBOARD) memcpy(rpc.keyboard, payload, sizeof(rpc.keyboard));
    else if (operation == RPC_MOUSE) {
        const mouse_args_t *mouse = payload;
        rpc.buttons = mouse->buttons; rpc.dx = mouse->dx; rpc.dy = mouse->dy;
        rpc.wheel = mouse->wheel; rpc.pan = mouse->pan;
    } else if (operation == RPC_CONSUMER) rpc.usage = *(const uint16_t *)payload;
    else if (operation == RPC_PAIR_REPLY) rpc.pair_reply = *(const pair_reply_args_t *)payload;
    ble_npl_mutex_release(&rpc.mutex);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &rpc.event);
    bool completed = ble_npl_sem_pend(&rpc.completion,
                      ble_npl_time_ms_to_ticks32(RPC_TIMEOUT_MS)) == BLE_NPL_OK;
    if (!completed) atomic_store(&rpc.cancelled, true);
    if (ble_npl_mutex_pend(&rpc.mutex, 0) != BLE_NPL_OK) {
        hid_guest_request_disconnect();
        return false;
    }
    bool result = completed && rpc.result;
    if (!completed) {
        if (rpc.finished) rpc.busy = false;
    }
    else rpc.busy = false;
    ble_npl_mutex_release(&rpc.mutex);
    if (!completed) hid_guest_request_disconnect();
    return result;
}

bool hid_guest_request_ready(void) { return request(RPC_READY, NULL); }
bool hid_guest_request_arm(void) { return request(RPC_ARM, NULL); }
bool hid_guest_request_release(void) { return request(RPC_RELEASE, NULL); }
bool hid_guest_request_keyboard(const uint8_t keys[HID_KEYBOARD_REPORT_LEN])
{ return request(RPC_KEYBOARD, keys); }
bool hid_guest_request_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                             int8_t wheel, int8_t pan)
{
    const mouse_args_t mouse = {buttons, dx, dy, wheel, pan};
    return request(RPC_MOUSE, &mouse);
}
bool hid_guest_request_consumer(uint16_t usage) { return request(RPC_CONSUMER, &usage); }
bool hid_guest_request_pair_begin(void) { return request(RPC_PAIR_BEGIN, NULL); }
bool hid_guest_request_pair_cancel(void) { return request(RPC_PAIR_CANCEL, NULL); }
bool hid_guest_request_pair_reply(uint32_t challenge_id, bool approved)
{
    const pair_reply_args_t args = {challenge_id, approved};
    return request(RPC_PAIR_REPLY, &args);
}
