/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Projects serialized firmware transport, pairing, router, and authenticated
 * HID facts into the device screen model. It clears stale session and numeric
 * challenge state and never invents update, recovery, or touch producers. */
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
    bool bonded_peer = has_token(core->connected_token);
    status->guest_slots = bonded_peer ? 1 : 0;
    status->guest_ready = bonded_peer && hid_ready;
    status->ready_slots = status->guest_ready ? 1 : 0;
    status->armed = router->armed && router->slot == 1 && status->guest_ready;
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
