/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE. */
#include "hardware_route.h"
#include <string.h>

static bool all_up(const kvm_hardware_route_t *route)
{
    return route->keyboard_up && route->pointer_up;
}

static void fail_local(kvm_hardware_route_t *route)
{
    uint8_t previous = route->target;
    route->armed = false;
    route->target = 0;
    route->generation++;
    if (route->output.release &&
        !route->output.release(route->context, previous) &&
        route->output.disconnect)
        route->output.disconnect(route->context, previous);
}

static bool arm_after_release(kvm_hardware_route_t *route)
{
    if (!all_up(route) || !route->output.ready ||
        !route->output.ready(route->context, route->target) ||
        !route->output.release) return false;
    if (!route->output.release(route->context, route->target)) {
        fail_local(route);
        return false;
    }
    if (!route->output.arm || !route->output.arm(route->context, route->target)) {
        fail_local(route);
        return false;
    }
    route->armed = true;
    return true;
}

void kvm_hardware_route_init(kvm_hardware_route_t *route,
                             kvm_hardware_output_t output, void *context)
{
    if (!route) return;
    memset(route, 0, sizeof(*route));
    route->output = output;
    route->context = context;
    route->keyboard_up = true;
    route->pointer_up = true;
}

bool kvm_hardware_route_select(kvm_hardware_route_t *route, uint8_t target)
{
    if (!route || target > KVM_ROUTER_MAX_SLOTS || !route->output.ready ||
        !route->output.ready(route->context, target)) return false;
    if (target == route->target && route->armed) return true;
    route->armed = false;
    if (!route->output.release ||
        !route->output.release(route->context, route->target)) {
        uint8_t previous = route->target;
        route->target = 0;
        route->generation++;
        if (route->output.disconnect)
            route->output.disconnect(route->context, previous);
        return false;
    }
    route->target = target;
    route->generation++;
    /* A held key/button is ignored until both source devices report all-up. */
    if (all_up(route)) return arm_after_release(route);
    return true;
}

bool kvm_hardware_route_input(kvm_hardware_route_t *route,
                              const kvm_router_input_t *input)
{
    if (!route || !input) return false;
    if (input->kind == KVM_ROUTER_KEYBOARD) {
        route->keyboard_up = true;
        for (unsigned i = 0; i < sizeof(input->keyboard); ++i)
            if (input->keyboard[i]) route->keyboard_up = false;
    } else if (input->kind == KVM_ROUTER_POINTER) {
        route->pointer_up = input->buttons == 0;
    } else if (input->kind != KVM_ROUTER_CONSUMER) return false;
    if (!route->armed) {
        if (all_up(route)) (void)arm_after_release(route);
        return false;
    }
    if (!route->output.ready || !route->output.ready(route->context, route->target) ||
        !route->output.send ||
        !route->output.send(route->context, route->target, input)) {
        fail_local(route);
        return false;
    }
    return true;
}

void kvm_hardware_route_input_lost(kvm_hardware_route_t *route)
{
    if (!route) return;
    fail_local(route);
    route->keyboard_up = false;
    route->pointer_up = false;
}

void kvm_hardware_route_output_lost(kvm_hardware_route_t *route, uint8_t target)
{
    if (route && target == route->target) fail_local(route);
}
