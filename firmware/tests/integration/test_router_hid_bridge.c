/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies that the router output adapter forwards each report to the bounded
 * NimBLE-host request API and requests disconnect after uncertain release. */
#include "router_hid_bridge.h"
#include "hid_guest.h"
#include <assert.h>

static int ready_calls, arms, releases, keyboards, mice, consumers, disconnects;
static uint8_t last_slot;
bool hid_guest_request_ready_slot(uint8_t slot) { last_slot = slot; ready_calls++; return true; }
bool hid_guest_request_arm_slot(uint8_t slot) { last_slot = slot; arms++; return true; }
bool hid_guest_request_release_slot(uint8_t slot) { last_slot = slot; releases++; return true; }
bool hid_guest_request_keyboard_slot(uint8_t slot, const uint8_t keys[8])
{ last_slot = slot; keyboards++; return keys[2] == 4; }
bool hid_guest_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{ last_slot = slot; mice++; return buttons == 1 && dx == -2 && dy == 3 && wheel == -1 && pan == 2; }
bool hid_guest_request_consumer_slot(uint8_t slot, uint16_t usage)
{ last_slot = slot; consumers++; return usage == 42; }
esp_err_t hid_guest_request_disconnect_slot(uint8_t slot)
{ last_slot = slot; disconnects++; return 0; }
bool hid_guest_request_ready(void) { ready_calls++; return true; }
bool hid_guest_request_arm(void) { arms++; return true; }
bool hid_guest_request_release(void) { releases++; return true; }
bool hid_guest_request_keyboard(const uint8_t keys[8]) { keyboards++; return keys[2] == 4; }
bool hid_guest_request_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{ mice++; return buttons == 1 && dx == -2 && dy == 3 && wheel == -1 && pan == 2; }
bool hid_guest_request_consumer(uint16_t usage) { consumers++; return usage == 42; }
esp_err_t hid_guest_request_disconnect(void) { disconnects++; return 0; }

int main(void)
{
    kvm_router_output_t out = kvm_router_hid_output();
    assert(!out.ready(NULL, 0) && ready_calls == 0);
    assert(out.ready(NULL, 1) && ready_calls == 1);
    assert(out.arm(NULL, 1) && arms == 1);
    kvm_router_input_t report = {0};
    report.kind = KVM_ROUTER_KEYBOARD; report.keyboard[2] = 4;
    assert(out.send(NULL, 1, &report) && keyboards == 1);
    report.kind = KVM_ROUTER_POINTER; report.buttons = 1;
    report.dx = -2; report.dy = 3; report.wheel = -1; report.pan = 2;
    assert(out.send(NULL, 1, &report) && mice == 1);
    report.kind = KVM_ROUTER_CONSUMER; report.consumer = 42;
    assert(out.send(NULL, 1, &report) && consumers == 1);
    assert(out.release(NULL, 1) && releases == 1);
    out.disconnect(NULL, 1);
    assert(disconnects == 1);
    assert(!out.send(NULL, 0, &report) && !out.arm(NULL, 0));
    assert(out.ready(NULL, 2) && last_slot == 2);
    assert(out.arm(NULL, 3) && last_slot == 3);
    report.kind = KVM_ROUTER_KEYBOARD; report.keyboard[2] = 4;
    assert(out.send(NULL, 2, &report) && last_slot == 2);
    assert(out.release(NULL, 3) && last_slot == 3);
    out.disconnect(NULL, 2);
    assert(last_slot == 2);
    return 0;
}
