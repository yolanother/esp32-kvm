/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies that the router output adapter forwards each report to the bounded
 * NimBLE-host request API and requests disconnect after uncertain release. */
#include "router_hid_bridge.h"
#include "doubtech_hid.h"
#include <assert.h>
#include <string.h>

static int ready_calls, arms, releases, keyboards, mice, consumers, disconnects;
static uint8_t last_slot;
bool doubtech_hid_request_ready_slot(uint8_t slot) { last_slot = slot; ready_calls++; return true; }
bool doubtech_hid_request_arm_slot(uint8_t slot) { last_slot = slot; arms++; return true; }
bool doubtech_hid_request_release_slot(uint8_t slot) { last_slot = slot; releases++; return true; }
bool doubtech_hid_request_keyboard_slot(uint8_t slot, const uint8_t keys[8])
{ last_slot = slot; keyboards++; return keys[2] == 4; }
bool doubtech_hid_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{ last_slot = slot; mice++; return buttons == 1 && dx == -2 && dy == 3 && wheel == -1 && pan == 2; }
bool doubtech_hid_request_consumer_slot(uint8_t slot, uint16_t usage)
{ last_slot = slot; consumers++; return usage == 42; }
esp_err_t doubtech_hid_request_disconnect_slot(uint8_t slot)
{ last_slot = slot; disconnects++; return 0; }
bool doubtech_hid_request_ready(void) { ready_calls++; return true; }
bool doubtech_hid_request_arm(void) { arms++; return true; }
bool doubtech_hid_request_release(void) { releases++; return true; }
bool doubtech_hid_request_keyboard(const uint8_t keys[8]) { keyboards++; return keys[2] == 4; }
bool doubtech_hid_request_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{ mice++; return buttons == 1 && dx == -2 && dy == 3 && wheel == -1 && pan == 2; }
bool doubtech_hid_request_consumer(uint16_t usage) { consumers++; return usage == 42; }
esp_err_t doubtech_hid_request_disconnect(void) { disconnects++; return 0; }

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

    /* Central reports become hub messages; the conversion only copies. */
    doubtech_hid_central_input_t central = {0};
    kvm_hub_msg_t msg;
    central.kind = DOUBTECH_HID_CENTRAL_KEYBOARD;
    central.keys[0] = 0x02; central.keys[2] = 4;
    assert(kvm_router_hid_central_msg(&central, &msg));
    assert(msg.type == KVM_HUB_MSG_INPUT && msg.input.kind == KVM_ROUTER_KEYBOARD &&
           msg.input.keyboard[0] == 0x02 && msg.input.keyboard[2] == 4);
    memset(&central, 0, sizeof central);
    central.kind = DOUBTECH_HID_CENTRAL_MOUSE;
    central.buttons = 3; central.dx = -300; central.dy = 7; central.wheel = -1;
    assert(kvm_router_hid_central_msg(&central, &msg));
    assert(msg.type == KVM_HUB_MSG_INPUT && msg.input.kind == KVM_ROUTER_POINTER &&
           msg.input.buttons == 3 && msg.input.dx == -300 && msg.input.dy == 7 &&
           msg.input.wheel == -1 && msg.input.pan == 0);
    memset(&central, 0, sizeof central);
    central.kind = DOUBTECH_HID_CENTRAL_MOUSE;
    central.lost = true;
    assert(kvm_router_hid_central_msg(&central, &msg) && msg.type == KVM_HUB_MSG_LOST);
    central.kind = 9; central.lost = false;
    assert(!kvm_router_hid_central_msg(&central, &msg));
    assert(!kvm_router_hid_central_msg(NULL, &msg));
    return 0;
}
