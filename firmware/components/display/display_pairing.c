/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the USB-independent pairing state for the local touch display.
 * It binds approval or rejection to the one live six-digit challenge and
 * clears comparison data after reply, expiry, cancellation, or release. */
#include "display_pairing.h"
#include <string.h>

void kvm_display_pairing_started(kvm_display_pairing_t *p, bool local_owner,
                                 uint64_t now_ms)
{
    if (!p) return;
    *p = (kvm_display_pairing_t){.local_owner = local_owner,
                                  .state = KVM_DISPLAY_PAIRING_WAITING,
                                  .deadline_ms = now_ms + 60000u};
}

bool kvm_display_pairing_event(kvm_display_pairing_t *p,
                               kvm_display_pairing_state_t state, uint32_t challenge_id,
                               uint32_t number, uint64_t deadline_ms, uint64_t now_ms)
{
    if (!p) return false;
    if (state == KVM_DISPLAY_PAIRING_CHALLENGE) {
        if (p->state != KVM_DISPLAY_PAIRING_WAITING || !challenge_id ||
            number > 999999u || deadline_ms <= now_ms ||
            deadline_ms > p->deadline_ms) return false;
        p->challenge_id = challenge_id;
        p->number = number;
        p->deadline_ms = deadline_ms;
        p->state = state;
        return true;
    }
    if (state == KVM_DISPLAY_PAIRING_CLOSED) {
        kvm_display_pairing_clear(p);
        return true;
    }
    if (state != KVM_DISPLAY_PAIRING_WAITING &&
        state != KVM_DISPLAY_PAIRING_REJECTED &&
        state != KVM_DISPLAY_PAIRING_CAPACITY &&
        state != KVM_DISPLAY_PAIRING_TIMEOUT) return false;
    p->challenge_id = 0;
    p->number = 0;
    p->state = state;
    if (state == KVM_DISPLAY_PAIRING_WAITING) p->deadline_ms = deadline_ms;
    return true;
}

bool kvm_display_pairing_accept(const kvm_display_pairing_t *p,
                                kvm_display_pair_request_t request, uint64_t now_ms)
{
    if (!p) return false;
    switch (request.action) {
    case KVM_DISPLAY_PAIR_BEGIN:
        return !request.challenge_id &&
            (p->state == KVM_DISPLAY_PAIRING_CLOSED ||
             p->state == KVM_DISPLAY_PAIRING_REJECTED ||
             p->state == KVM_DISPLAY_PAIRING_TIMEOUT);
    case KVM_DISPLAY_PAIR_CANCEL:
        return p->local_owner && !request.challenge_id &&
            p->state == KVM_DISPLAY_PAIRING_WAITING &&
            p->deadline_ms > now_ms;
    case KVM_DISPLAY_PAIR_APPROVE:
    case KVM_DISPLAY_PAIR_REJECT:
        return p->local_owner && p->state == KVM_DISPLAY_PAIRING_CHALLENGE && p->challenge_id &&
            request.challenge_id == p->challenge_id && p->number <= 999999u &&
            p->deadline_ms > now_ms;
    default: return false;
    }
}

void kvm_display_pairing_replied(kvm_display_pairing_t *p, bool approved)
{
    if (!p) return;
    p->challenge_id = 0;
    p->number = 0;
    p->state = approved ? KVM_DISPLAY_PAIRING_WAITING : KVM_DISPLAY_PAIRING_REJECTED;
}

void kvm_display_pairing_expire(kvm_display_pairing_t *p, uint64_t now_ms)
{
    if (!p || (p->state != KVM_DISPLAY_PAIRING_WAITING &&
               p->state != KVM_DISPLAY_PAIRING_CHALLENGE) ||
        p->deadline_ms > now_ms) return;
    p->challenge_id = 0;
    p->number = 0;
    p->state = KVM_DISPLAY_PAIRING_TIMEOUT;
}

void kvm_display_pairing_clear(kvm_display_pairing_t *p)
{
    if (p) memset(p, 0, sizeof(*p));
}
