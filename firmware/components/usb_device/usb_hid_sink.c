/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure, state-based USB HID sink: latest-state keyboard and
 * consumer reports, saturating pointer accumulators emitted as clamped chunks,
 * and a non-blocking pump that sends at most one report per interface and
 * clears a dirty flag only once the port accepted the report. */
#include "usb_hid_sink.h"

#include <string.h>

#include "usb_descriptors.h"

static int32_t add_sat32(int32_t acc, int32_t delta)
{
    int64_t sum = (int64_t)acc + delta;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

static int16_t add_sat16(int16_t acc, int32_t delta)
{
    int32_t sum = (int32_t)acc + delta;
    if (sum > INT16_MAX) return INT16_MAX;
    if (sum < INT16_MIN) return INT16_MIN;
    return (int16_t)sum;
}

static int32_t clamp(int32_t v, int32_t limit)
{
    if (v > limit) return limit;
    if (v < -limit) return -limit;
    return v;
}

static bool motion_pending(const kvm_usb_hid_sink_t *s)
{
    return s->dx != 0 || s->dy != 0 || s->wheel != 0 || s->pan != 0;
}

static bool pointer_owed(const kvm_usb_hid_sink_t *s)
{
    return s->pointer_dirty || motion_pending(s);
}

void kvm_usb_hid_sink_init(kvm_usb_hid_sink_t *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->consumer_next = true;
}

void kvm_usb_hid_sink_apply(kvm_usb_hid_sink_t *s, const kvm_router_input_t *in)
{
    if (s == NULL || in == NULL) return;
    switch (in->kind) {
    case KVM_ROUTER_KEYBOARD:
        memcpy(s->keys, in->keyboard, sizeof(s->keys));
        s->keys_dirty = true;
        break;
    case KVM_ROUTER_POINTER:
        s->buttons = in->buttons;
        s->dx = add_sat32(s->dx, in->dx);
        s->dy = add_sat32(s->dy, in->dy);
        s->wheel = add_sat16(s->wheel, in->wheel);
        s->pan = add_sat16(s->pan, in->pan);
        s->pointer_dirty = true;
        break;
    case KVM_ROUTER_CONSUMER:
        s->consumer = in->consumer;
        s->consumer_dirty = true;
        break;
    default:
        break;
    }
}

void kvm_usb_hid_sink_release_all(kvm_usb_hid_sink_t *s)
{
    if (s == NULL) return;
    memset(s->keys, 0, sizeof(s->keys));
    s->buttons = 0;
    s->consumer = 0;
    s->dx = s->dy = 0;
    s->wheel = s->pan = 0;
    s->keys_dirty = s->pointer_dirty = s->consumer_dirty = true;
}

void kvm_usb_hid_sink_abandon(kvm_usb_hid_sink_t *s)
{
    kvm_usb_hid_sink_init(s);
}

void kvm_usb_hid_keyboard_report(const kvm_usb_hid_sink_t *s, uint8_t out[8])
{
    if (out == NULL) return;
    if (s == NULL) {
        memset(out, 0, KVM_USB_HID_KEYBOARD_REPORT_LEN);
        return;
    }
    memcpy(out, s->keys, KVM_USB_HID_KEYBOARD_REPORT_LEN);
}

static void put16(uint8_t *p, int32_t v)
{
    uint16_t u = (uint16_t)(int16_t)v;
    p[0] = (uint8_t)(u & 0xFF);
    p[1] = (uint8_t)(u >> 8);
}

bool kvm_usb_hid_pointer_report(kvm_usb_hid_sink_t *s, uint8_t out[7])
{
    if (out == NULL) return false;
    memset(out, 0, KVM_USB_HID_POINTER_REPORT_LEN);
    if (s == NULL || !pointer_owed(s)) return false;
    int32_t x = clamp(s->dx, KVM_USB_HID_AXIS_MAX);
    int32_t y = clamp(s->dy, KVM_USB_HID_AXIS_MAX);
    int32_t w = clamp(s->wheel, KVM_USB_HID_WHEEL_MAX);
    int32_t h = clamp(s->pan, KVM_USB_HID_WHEEL_MAX);
    out[0] = (uint8_t)(s->buttons & KVM_USB_HID_BUTTON_MASK);
    put16(out + 1, x);
    put16(out + 3, y);
    out[5] = (uint8_t)(int8_t)w;
    out[6] = (uint8_t)(int8_t)h;
    s->dx -= x;
    s->dy -= y;
    s->wheel = (int16_t)(s->wheel - w);
    s->pan = (int16_t)(s->pan - h);
    s->pointer_dirty = motion_pending(s);
    return true;
}

void kvm_usb_hid_consumer_report(const kvm_usb_hid_sink_t *s, uint8_t out[2])
{
    if (out == NULL) return;
    uint16_t usage = s == NULL ? 0 : s->consumer;
    out[0] = (uint8_t)(usage & 0xFF);
    out[1] = (uint8_t)(usage >> 8);
}

static bool port_ready(const kvm_usb_hid_port_t *port, uint8_t itf)
{
    return port->ready == NULL || port->ready(port->context, itf);
}

bool kvm_usb_hid_sink_pump(kvm_usb_hid_sink_t *s, const kvm_usb_hid_port_t *port)
{
    if (s == NULL) return true;
    if (port == NULL || port->send == NULL) return !kvm_usb_hid_sink_pending(s);

    if (s->keys_dirty && port_ready(port, KVM_USB_ITF_KEYBOARD)) {
        uint8_t report[KVM_USB_HID_KEYBOARD_REPORT_LEN];
        kvm_usb_hid_keyboard_report(s, report);
        if (port->send(port->context, KVM_USB_ITF_KEYBOARD, 0, report, sizeof(report))) s->keys_dirty = false;
    }

    bool want_pointer = pointer_owed(s);
    if ((s->consumer_dirty || want_pointer) && port_ready(port, KVM_USB_ITF_POINTER)) {
        bool consumer = s->consumer_dirty && (!want_pointer || s->consumer_next);
        /* The other report kind is preferred next, so the shared endpoint never starves one. */
        s->consumer_next = !consumer;
        if (consumer) {
            uint8_t report[KVM_USB_HID_CONSUMER_REPORT_LEN];
            kvm_usb_hid_consumer_report(s, report);
            if (port->send(port->context, KVM_USB_ITF_POINTER, KVM_USB_REPORT_ID_CONSUMER, report, sizeof(report)))
                s->consumer_dirty = false;
        } else {
            /* Take the chunk on a copy so a refused send consumes no motion. */
            kvm_usb_hid_sink_t next = *s;
            uint8_t report[KVM_USB_HID_POINTER_REPORT_LEN];
            if (kvm_usb_hid_pointer_report(&next, report) &&
                port->send(port->context, KVM_USB_ITF_POINTER, KVM_USB_REPORT_ID_MOUSE, report, sizeof(report))) {
                s->dx = next.dx;
                s->dy = next.dy;
                s->wheel = next.wheel;
                s->pan = next.pan;
                s->pointer_dirty = next.pointer_dirty;
            }
        }
    }
    return !kvm_usb_hid_sink_pending(s);
}

bool kvm_usb_hid_sink_pending(const kvm_usb_hid_sink_t *s)
{
    if (s == NULL) return false;
    return s->keys_dirty || s->consumer_dirty || pointer_owed(s);
}
