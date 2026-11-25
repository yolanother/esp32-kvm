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
    KVM_HUB_TARGET_CHANGED
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
} kvm_input_hub_t;

/** Initializes the hub on target zero with nothing armed or held. */
void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context);
/** Selects target 0 to 3, sending all-up and releasing the old target first. Returns false
 * and keeps the old target when the target is invalid or the all-up or release failed. */
bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target);
/** Delivers one report; arms the target lazily. While router_armed it drops the report,
 * clears local hub state and emits nothing. */
kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed);
/** Sends all-up then releases the arm. The caller must invoke it BEFORE the router arms;
 * the hub cannot see the router slot and never emits while the router is armed. */
void kvm_input_hub_yield(kvm_input_hub_t *hub);
/** Sends all-up keyboard, zero pointer and zero consumer after an input device is lost. */
void kvm_input_hub_device_lost(kvm_input_hub_t *hub);

#endif
