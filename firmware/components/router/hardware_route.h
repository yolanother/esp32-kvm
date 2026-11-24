/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Device-owned input routing policy. BLE input and the selected output remain
 * independent of the Windows CDC session and desktop application. */
#ifndef ESP32_KVM_HARDWARE_ROUTE_H
#define ESP32_KVM_HARDWARE_ROUTE_H

#include "router.h"
#include <stdbool.h>
#include <stdint.h>

/* Target zero is the Windows USB HID output; 1..3 are BLE HID guests. */
typedef struct {
    bool (*ready)(void *context, uint8_t target);
    bool (*release)(void *context, uint8_t target);
    bool (*arm)(void *context, uint8_t target);
    bool (*send)(void *context, uint8_t target, const kvm_router_input_t *input);
    void (*disconnect)(void *context, uint8_t target);
} kvm_hardware_output_t;

typedef struct {
    kvm_hardware_output_t output;
    void *context;
    uint8_t target;
    uint32_t generation;
    bool armed;
    bool keyboard_up;
    bool pointer_up;
} kvm_hardware_route_t;

/* Starts disarmed on USB/local. Call from one serialized firmware owner. */
void kvm_hardware_route_init(kvm_hardware_route_t *route,
                             kvm_hardware_output_t output, void *context);
/* Releases the old output before selecting a ready target. */
bool kvm_hardware_route_select(kvm_hardware_route_t *route, uint8_t target);
/* Full state reports from bonded, authenticated BLE input devices only. */
bool kvm_hardware_route_input(kvm_hardware_route_t *route,
                              const kvm_router_input_t *input);
/* A lost input source releases held state and requires fresh all-up reports. */
void kvm_hardware_route_input_lost(kvm_hardware_route_t *route);
/* Windows USB loss cannot disturb a selected BLE guest. */
void kvm_hardware_route_output_lost(kvm_hardware_route_t *route, uint8_t target);

#endif
