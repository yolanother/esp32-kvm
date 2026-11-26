/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Projects serialized firmware transport, pairing, router, and authenticated
 * HID facts into the device screen model. It clears stale session and numeric
 * challenge state and never invents update, recovery, or touch producers.
 * It also holds the pure worker side of the USB mode screen: the boot facts
 * projection, the bounded request queue and the decision for each request. */
#include "transport_display_status.h"
#include <string.h>

static bool has_token(const uint8_t token[16])
{
    for (size_t i = 0; i < 16; ++i)
        if (token[i]) return true;
    return false;
}

void kvm_transport_display_status(const kvm_transport_core_t *core,
                                  const kvm_router_t *router, bool usb_connected,
                                  bool hid_ready, uint64_t now_ms,
                                  kvm_display_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!core || !router || !usb_connected || !core->session_open ||
        !router->session_open) return;

    status->usb_connected = true;
    status->selected_slot = router->slot;
    status->generation = router->generation;
    bool selected_ready = false;
    for (uint8_t slot = 1; slot <= KVM_ROUTER_MAX_SLOTS; ++slot) {
        const kvm_transport_slot_t *item = &core->slots[slot - 1];
        if (!has_token(item->token)) continue;
        status->occupied_mask |= (uint8_t)(1u << (slot - 1u));
        status->guest_slots = slot;
        bool ready = item->ready && item->subscribed;
        if (ready) {
            status->ready_slots++;
            status->ready_mask |= (uint8_t)(1u << (slot - 1u));
        }
        if (router->slot == slot) selected_ready = ready;
    }
    if (!status->guest_slots && has_token(core->connected_token)) {
        status->guest_slots = 1;
        status->ready_slots = hid_ready ? 1 : 0;
        status->ready_mask = hid_ready ? 1 : 0;
        selected_ready = router->slot == 1 && hid_ready;
    }
    status->guest_ready = router->slot ? selected_ready : status->ready_slots > 0;
    status->armed = router->armed && selected_ready;
    status->fault = router->fault || (router->armed && !status->armed);

    if (core->minor < 1) return;
    switch (core->pairing_state) {
    case KVM_PAIRING_WAITING:
    case KVM_PAIRING_CHALLENGE:
        if (now_ms >= core->pairing_deadline_ms) {
            status->pairing_state = KVM_DISPLAY_PAIRING_TIMEOUT;
            return;
        }
        if (core->pairing_state == KVM_PAIRING_CHALLENGE) {
            if (!core->pairing_challenge_id || core->pairing_number > 999999u) {
                status->pairing_state = KVM_DISPLAY_PAIRING_REJECTED;
                return;
            }
            status->pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
            status->pairing_challenge_id = core->pairing_challenge_id;
            status->pairing_number = core->pairing_number;
        } else status->pairing_state = KVM_DISPLAY_PAIRING_WAITING;
        status->pairing_deadline_ms = core->pairing_deadline_ms;
        return;
    case KVM_PAIRING_REJECTED: status->pairing_state = KVM_DISPLAY_PAIRING_REJECTED; return;
    case KVM_PAIRING_CAPACITY: status->pairing_state = KVM_DISPLAY_PAIRING_CAPACITY; return;
    case KVM_PAIRING_TIMEOUT: status->pairing_state = KVM_DISPLAY_PAIRING_TIMEOUT; return;
    default: return;
    }
}

void kvm_transport_display_usb(uint8_t mode, uint8_t reason, uint8_t pc_hid,
                               kvm_display_status_t *status)
{
    if (!status) return;
    status->usb_mode = mode == 1u ? 1u : 0u;
    status->usb_reason = reason;
    status->pc_hid = status->usb_mode && pc_hid <= 2u ? pc_hid : 0u;
}

static bool usb_request_valid(kvm_display_usb_request_t request)
{
    return request == KVM_DISPLAY_USB_USE_COMPOSITE || request == KVM_DISPLAY_USB_USE_SERIAL ||
           request == KVM_DISPLAY_USB_SERIAL_ONCE;
}

bool kvm_transport_usb_queue_push(kvm_transport_usb_queue_t *q, kvm_display_usb_request_t request)
{
    if (!q || !usb_request_valid(request)) return false;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < q->count && i < KVM_TRANSPORT_USB_QUEUE_DEPTH; ++i)
        if (q->items[i] != (uint8_t)request) q->items[kept++] = q->items[i];
    if (kept >= KVM_TRANSPORT_USB_QUEUE_DEPTH) return false; /* unreachable: three values */
    q->items[kept++] = (uint8_t)request;
    q->count = kept;
    return true;
}

bool kvm_transport_usb_queue_pop(kvm_transport_usb_queue_t *q, kvm_display_usb_request_t *request)
{
    if (!q || !request || q->count == 0) return false;
    *request = (kvm_display_usb_request_t)q->items[0];
    uint8_t count = q->count <= KVM_TRANSPORT_USB_QUEUE_DEPTH ? q->count : KVM_TRANSPORT_USB_QUEUE_DEPTH;
    for (uint8_t i = 1; i < count; ++i) q->items[i - 1u] = q->items[i];
    q->count = (uint8_t)(count - 1u);
    return true;
}

kvm_transport_usb_action_t kvm_transport_usb_action(kvm_display_usb_request_t request,
                                                    bool composite_supported,
                                                    bool host_armed, bool guest_pairing)
{
    if (!usb_request_valid(request)) return KVM_TRANSPORT_USB_ACTION_NONE;
    if (request == KVM_DISPLAY_USB_USE_COMPOSITE && !composite_supported)
        return KVM_TRANSPORT_USB_ACTION_NOT_BUILT;
    if (host_armed || guest_pairing) return KVM_TRANSPORT_USB_ACTION_BUSY;
    switch (request) {
    case KVM_DISPLAY_USB_USE_COMPOSITE: return KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE;
    case KVM_DISPLAY_USB_USE_SERIAL: return KVM_TRANSPORT_USB_ACTION_SERIAL_MODE;
    default: return KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE;
    }
}

uint8_t kvm_transport_usb_action_result(kvm_transport_usb_action_t action)
{
    switch (action) {
    case KVM_TRANSPORT_USB_ACTION_NOT_BUILT: return KVM_DISPLAY_USB_RESULT_NOT_BUILT;
    case KVM_TRANSPORT_USB_ACTION_BUSY: return KVM_DISPLAY_USB_RESULT_BUSY;
    case KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE:
    case KVM_TRANSPORT_USB_ACTION_SERIAL_MODE:
    case KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE: return KVM_DISPLAY_USB_RESULT_RESTARTING;
    default: return KVM_DISPLAY_USB_RESULT_NONE;
    }
}

bool kvm_transport_usb_action_reboots(kvm_transport_usb_action_t action)
{
    return action == KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE ||
           action == KVM_TRANSPORT_USB_ACTION_SERIAL_MODE ||
           action == KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE;
}
