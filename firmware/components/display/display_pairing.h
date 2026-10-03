/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Tracks the device screen's pairing-window ownership and exact pending numeric
 * challenge independently of a USB session. The USB worker uses this state to
 * reject stale touch requests before calling bounded NimBLE host requests. */
#ifndef ESP32_KVM_DISPLAY_PAIRING_H
#define ESP32_KVM_DISPLAY_PAIRING_H

#include "display_model.h"

/** Pairing state owned by the transport worker, with no retained pairing secret. */
typedef struct {
    bool local_owner;
    kvm_display_pairing_state_t state;
    uint32_t challenge_id;
    uint32_t number;
    uint64_t deadline_ms;
} kvm_display_pairing_t;

/** Records a successfully opened 60-second pairing window. */
void kvm_display_pairing_started(kvm_display_pairing_t *pairing, bool local_owner,
                                 uint64_t now_ms);
/** Applies an authenticated HID pairing event, rejecting malformed challenges. */
bool kvm_display_pairing_event(kvm_display_pairing_t *pairing,
                               kvm_display_pairing_state_t state, uint32_t challenge_id,
                               uint32_t number, uint64_t deadline_ms, uint64_t now_ms);
/** Returns whether a queued touch action still matches the current window. */
bool kvm_display_pairing_accept(const kvm_display_pairing_t *pairing,
                                kvm_display_pair_request_t request, uint64_t now_ms);
/** Marks a completed local reply so the challenge cannot be replayed. */
void kvm_display_pairing_replied(kvm_display_pairing_t *pairing, bool approved);
/** Expires a window without exposing a stale comparison number. */
void kvm_display_pairing_expire(kvm_display_pairing_t *pairing, uint64_t now_ms);
/** Clears state after cancellation or emergency release. */
void kvm_display_pairing_clear(kvm_display_pairing_t *pairing);

#endif
