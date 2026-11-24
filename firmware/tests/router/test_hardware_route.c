/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE. */
#include "hardware_route.h"
#include <assert.h>
#include <string.h>

typedef struct {
    bool ready[4];
    bool release_ok;
    unsigned releases[4];
    unsigned arms[4];
    unsigned sends[4];
    unsigned disconnects[4];
} sink_t;

static bool ready(void *context, uint8_t target)
{ return ((sink_t *)context)->ready[target]; }
static bool release(void *context, uint8_t target)
{
    sink_t *sink = context;
    sink->releases[target]++;
    return sink->release_ok;
}
static bool arm(void *context, uint8_t target)
{
    ((sink_t *)context)->arms[target]++;
    return true;
}
static bool send(void *context, uint8_t target, const kvm_router_input_t *input)
{
    sink_t *sink = context;
    assert(input->kind == KVM_ROUTER_KEYBOARD || input->kind == KVM_ROUTER_POINTER);
    sink->sends[target]++;
    return true;
}
static void disconnect(void *context, uint8_t target)
{ ((sink_t *)context)->disconnects[target]++; }

static kvm_router_input_t keyboard(uint8_t key)
{
    kvm_router_input_t input = {0};
    input.kind = KVM_ROUTER_KEYBOARD;
    input.keyboard[2] = key;
    return input;
}

int main(void)
{
    sink_t sink = {0};
    for (unsigned i = 0; i < 4; ++i) sink.ready[i] = true;
    sink.release_ok = true;
    kvm_hardware_output_t output = { ready, release, arm, send, disconnect };
    kvm_hardware_route_t route;
    kvm_hardware_route_init(&route, output, &sink);

    /* Windows USB can vanish during reboot without losing the Mac route. */
    assert(kvm_hardware_route_select(&route, 1));
    assert(route.armed && route.target == 1);
    assert(sink.arms[1] == 1);
    kvm_router_input_t down = keyboard(4);
    assert(kvm_hardware_route_input(&route, &down));
    sink.ready[0] = false;
    kvm_hardware_route_output_lost(&route, 0);
    assert(route.armed && route.target == 1);
    assert(kvm_hardware_route_input(&route, &down));
    assert(sink.sends[1] == 2);

    /* A held key cannot leak into the next target on a switch. */
    sink.ready[0] = true;
    assert(kvm_hardware_route_select(&route, 0));
    assert(!route.armed && route.target == 0);
    assert(!kvm_hardware_route_input(&route, &down));
    assert(sink.sends[0] == 0);
    kvm_router_input_t up = keyboard(0);
    assert(!kvm_hardware_route_input(&route, &up));
    assert(route.armed);
    assert(kvm_hardware_route_input(&route, &down));
    assert(sink.sends[0] == 1);

    /* Losing either physical input source releases guest state. */
    assert(kvm_hardware_route_select(&route, 1));
    kvm_hardware_route_input_lost(&route);
    assert(!route.armed && route.target == 0);
    assert(!kvm_hardware_route_input(&route, &up));
    kvm_router_input_t pointer = {0};
    pointer.kind = KVM_ROUTER_POINTER;
    assert(!kvm_hardware_route_input(&route, &pointer));
    assert(route.armed && route.target == 0);

    /* A failed release disconnects the old guest instead of rearming it. */
    assert(kvm_hardware_route_select(&route, 1));
    sink.release_ok = false;
    assert(!kvm_hardware_route_select(&route, 0));
    assert(!route.armed && route.target == 0);
    assert(sink.disconnects[1] == 1);
    return 0;
}
