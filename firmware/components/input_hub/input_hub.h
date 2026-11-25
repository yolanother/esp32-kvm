/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the pure input hub: it routes locally attached input device reports
 * to one selected guest target and yields to the host-armed route. It holds
 * no platform state; the caller serializes all calls with the router actor. */
#ifndef ESP32_KVM_INPUT_HUB_H
#define ESP32_KVM_INPUT_HUB_H

#include "router.h"

/** Highest selectable target; 0 is the local sink, 1 to 3 are guest slots. */
#define KVM_INPUT_HUB_MAX_TARGET 3u

/** Outcome of delivering one device report. */
typedef enum {
    KVM_HUB_FORWARDED,
    KVM_HUB_SWALLOWED,
    KVM_HUB_DROPPED_HOST_ARMED,
    KVM_HUB_DROPPED_NOT_READY,
    KVM_HUB_TARGET_CHANGED,
    /** A chord was swallowed but releasing the held old state failed on a ready slot; it is
     * retried on each later swallowed keyboard report until it succeeds. */
    KVM_HUB_SWITCH_FAILED
} kvm_hub_result_t;

/** Hub state; armed_slot is zero while the hub holds no output arm. keys, buttons and
 * consumer are the last forwarded held state and stay set until an all-up is delivered. */
typedef struct {
    kvm_router_output_t output;
    void *context;
    uint8_t target;
    uint8_t armed_slot;
    uint8_t keys[8];
    uint8_t buttons;
    uint16_t consumer;
    uint8_t raw_keys[8]; /* last raw keyboard report, for chord press detection */
    bool chord_latched;  /* swallow keyboard reports until every non-modifier key is up */
    bool switch_pending; /* a chord switch or release is awaiting retry */
    uint8_t pending_target;
} kvm_input_hub_t;

/** Initializes the hub on target zero with nothing armed or held. */
void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context);
/** Re-arm policy: a retry never arms. All-up is only ever sent to a slot the hub itself holds
 * armed (held state implies an arm). Whenever router_armed is true, or the router arms, the
 * hub abandons its held state and arm without emitting, so no retry can touch a slot the host
 * route owns. A slot the output reports not ready is dead: it cannot hold keys down, so its
 * held state and arm are abandoned instead of retried; a failure on a ready slot keeps both.
 *
 * Selects target 0 to 3, sending all-up and releasing the old target first. Returns false
 * and keeps the old target only when the target is invalid or the all-up or release failed
 * on a ready old target (retry later). A not-ready old target never blocks the switch. */
bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed);
/** Delivers one report; arms the target lazily. While router_armed it drops the report,
 * clears local hub state and emits nothing. Keyboard reports with exactly Left Ctrl + Left Alt
 * (modifier byte 0x05) and a single newly pressed key select a target: 1 to 3 guests, 0 This
 * PC, Esc release all to This PC. The chord is swallowed whole, including the held key until
 * all non-modifier keys are up. Returns TARGET_CHANGED when the target changed, else SWALLOWED. */
kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed);
/** Sends all-up then releases the arm. The caller must invoke it BEFORE the router arms;
 * the hub cannot see the router slot. Returns true when nothing is left held or armed
* (a dead slot counts as released), false when a ready slot failed and a retry is needed.
 * With router_armed it abandons local state without emitting and returns true. */
bool kvm_input_hub_yield(kvm_input_hub_t *hub, bool router_armed);
/** Sends all-up keyboard, zero pointer and zero consumer after an input device is lost.
 * With router_armed it only abandons local state and emits nothing. */
void kvm_input_hub_device_lost(kvm_input_hub_t *hub, bool router_armed);

#endif
