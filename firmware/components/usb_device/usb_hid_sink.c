/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure USB HID sink: per-stream ordered FIFOs of distinct
 * keyboard, pointer and consumer states (replace-newest when full), pointer
 * motion coalesced only within one button state into saturating accumulators
 * and emitted as clamped chunks, and a non-blocking pump that sends at most
 * one report per interface and removes an entry only once the port accepted
 * its report. */
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

/* Ring index of the i-th entry (0 = oldest). */
static uint8_t ring(uint8_t head, uint8_t i, uint8_t depth) { return (uint8_t)((head + i) % depth); }

/* ---- keyboard ---------------------------------------------------------------------------- */

static void key_push(kvm_usb_hid_sink_t *s, const uint8_t state[8])
{
    const uint8_t depth = KVM_USB_HID_KEY_QUEUE_DEPTH;
    if (s->key_count < depth) s->key_count++; /* full: overwrite the newest entry */
    memcpy(s->key_queue[ring(s->key_head, (uint8_t)(s->key_count - 1), depth)], state, 8);
    memcpy(s->keys, state, 8);
}

static void key_pop(kvm_usb_hid_sink_t *s)
{
    s->key_head = ring(s->key_head, 1, KVM_USB_HID_KEY_QUEUE_DEPTH);
    s->key_count--;
}

/* ---- consumer ---------------------------------------------------------------------------- */

static void consumer_push(kvm_usb_hid_sink_t *s, uint16_t usage)
{
    const uint8_t depth = KVM_USB_HID_CONSUMER_QUEUE_DEPTH;
    if (s->consumer_count < depth) s->consumer_count++;
    s->consumer_queue[ring(s->consumer_head, (uint8_t)(s->consumer_count - 1), depth)] = usage;
    s->consumer = usage;
}

static void consumer_pop(kvm_usb_hid_sink_t *s)
{
    s->consumer_head = ring(s->consumer_head, 1, KVM_USB_HID_CONSUMER_QUEUE_DEPTH);
    s->consumer_count--;
}

/* ---- pointer ----------------------------------------------------------------------------- */

static kvm_usb_hid_pointer_entry_t *pointer_at(kvm_usb_hid_sink_t *s, uint8_t i)
{
    return &s->pointer_queue[ring(s->pointer_head, i, KVM_USB_HID_POINTER_QUEUE_DEPTH)];
}

static bool entry_moves(const kvm_usb_hid_pointer_entry_t *e)
{
    return e->dx != 0 || e->dy != 0 || e->wheel != 0 || e->pan != 0;
}

static void entry_add(kvm_usb_hid_pointer_entry_t *e, const kvm_router_input_t *in)
{
    e->dx = add_sat32(e->dx, in->dx);
    e->dy = add_sat32(e->dy, in->dy);
    e->wheel = add_sat16(e->wheel, in->wheel);
    e->pan = add_sat16(e->pan, in->pan);
}

static kvm_usb_hid_pointer_entry_t *pointer_push(kvm_usb_hid_sink_t *s, uint8_t buttons, bool transition)
{
    kvm_usb_hid_pointer_entry_t *e = pointer_at(s, (uint8_t)(s->pointer_count++));
    memset(e, 0, sizeof(*e));
    e->buttons = buttons;
    e->transition = transition;
    return e;
}

static void pointer_pop(kvm_usb_hid_sink_t *s)
{
    s->pointer_head = ring(s->pointer_head, 1, KVM_USB_HID_POINTER_QUEUE_DEPTH);
    s->pointer_count--;
}

static void apply_pointer(kvm_usb_hid_sink_t *s, const kvm_router_input_t *in)
{
    const uint8_t buttons = (uint8_t)(in->buttons & KVM_USB_HID_BUTTON_MASK);
    const bool moves = in->dx != 0 || in->dy != 0 || in->wheel != 0 || in->pan != 0;
    const bool full = s->pointer_count >= KVM_USB_HID_POINTER_QUEUE_DEPTH;
    kvm_usb_hid_pointer_entry_t *newest = s->pointer_count ? pointer_at(s, (uint8_t)(s->pointer_count - 1)) : NULL;

    if (buttons != s->buttons) {
        s->buttons = buttons;
        if (full) {
            /* Replace the newest entry: its button state becomes this one, motion is kept. */
            newest->buttons = buttons;
            newest->transition = true;
            entry_add(newest, in);
        } else {
            entry_add(pointer_push(s, buttons, true), in);
        }
        return;
    }
    if (!moves) return;
    if (newest != NULL && (!newest->transition || full)) {
        entry_add(newest, in);
        /* A motion-only entry whose motion cancelled out owes nothing. */
        if (!newest->transition && !entry_moves(newest)) s->pointer_count--;
    } else {
        entry_add(pointer_push(s, buttons, false), in);
    }
}

/* Computes the next chunk of the oldest entry into out without consuming it. */
static bool pointer_chunk(const kvm_usb_hid_sink_t *s, uint8_t out[7], int32_t chunk[4])
{
    memset(out, 0, KVM_USB_HID_POINTER_REPORT_LEN);
    if (s->pointer_count == 0) return false;
    const kvm_usb_hid_pointer_entry_t *e = &s->pointer_queue[s->pointer_head];
    chunk[0] = clamp(e->dx, KVM_USB_HID_AXIS_MAX);
    chunk[1] = clamp(e->dy, KVM_USB_HID_AXIS_MAX);
    chunk[2] = clamp(e->wheel, KVM_USB_HID_WHEEL_MAX);
    chunk[3] = clamp(e->pan, KVM_USB_HID_WHEEL_MAX);
    uint16_t x = (uint16_t)(int16_t)chunk[0], y = (uint16_t)(int16_t)chunk[1];
    out[0] = (uint8_t)(e->buttons & KVM_USB_HID_BUTTON_MASK);
    out[1] = (uint8_t)(x & 0xFF);
    out[2] = (uint8_t)(x >> 8);
    out[3] = (uint8_t)(y & 0xFF);
    out[4] = (uint8_t)(y >> 8);
    out[5] = (uint8_t)(int8_t)chunk[2];
    out[6] = (uint8_t)(int8_t)chunk[3];
    return true;
}

/* Subtracts a reported chunk from the oldest entry and removes it once nothing remains. */
static void pointer_commit(kvm_usb_hid_sink_t *s, const int32_t chunk[4])
{
    kvm_usb_hid_pointer_entry_t *e = &s->pointer_queue[s->pointer_head];
    e->dx -= chunk[0];
    e->dy -= chunk[1];
    e->wheel = (int16_t)(e->wheel - chunk[2]);
    e->pan = (int16_t)(e->pan - chunk[3]);
    e->transition = false; /* the button change itself has been reported */
    if (!entry_moves(e)) pointer_pop(s);
}

/* ---- public API -------------------------------------------------------------------------- */

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
        if (memcmp(in->keyboard, s->keys, sizeof(s->keys)) != 0) key_push(s, in->keyboard);
        break;
    case KVM_ROUTER_POINTER:
        apply_pointer(s, in);
        break;
    case KVM_ROUTER_CONSUMER:
        if (in->consumer != s->consumer) consumer_push(s, in->consumer);
        break;
    default:
        break;
    }
}

void kvm_usb_hid_sink_release_all(kvm_usb_hid_sink_t *s)
{
    if (s == NULL) return;
    static const uint8_t zero[8] = { 0 };
    s->key_count = s->pointer_count = s->consumer_count = 0;
    key_push(s, zero);
    consumer_push(s, 0);
    s->buttons = 0;
    pointer_push(s, 0, true);
}

void kvm_usb_hid_sink_abandon(kvm_usb_hid_sink_t *s)
{
    kvm_usb_hid_sink_init(s);
}

void kvm_usb_hid_keyboard_report(const kvm_usb_hid_sink_t *s, uint8_t out[8])
{
    if (out == NULL) return;
    if (s == NULL) memset(out, 0, KVM_USB_HID_KEYBOARD_REPORT_LEN);
    else if (s->key_count) memcpy(out, s->key_queue[s->key_head], KVM_USB_HID_KEYBOARD_REPORT_LEN);
    else memcpy(out, s->keys, KVM_USB_HID_KEYBOARD_REPORT_LEN);
}

bool kvm_usb_hid_pointer_report(kvm_usb_hid_sink_t *s, uint8_t out[7])
{
    if (out == NULL) return false;
    if (s == NULL) {
        memset(out, 0, KVM_USB_HID_POINTER_REPORT_LEN);
        return false;
    }
    int32_t chunk[4];
    if (!pointer_chunk(s, out, chunk)) return false;
    pointer_commit(s, chunk);
    return true;
}

void kvm_usb_hid_consumer_report(const kvm_usb_hid_sink_t *s, uint8_t out[2])
{
    if (out == NULL) return;
    uint16_t usage = 0;
    if (s != NULL) usage = s->consumer_count ? s->consumer_queue[s->consumer_head] : s->consumer;
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

    if (s->key_count && port_ready(port, KVM_USB_ITF_KEYBOARD)) {
        uint8_t report[KVM_USB_HID_KEYBOARD_REPORT_LEN];
        kvm_usb_hid_keyboard_report(s, report);
        if (port->send(port->context, KVM_USB_ITF_KEYBOARD, 0, report, sizeof(report))) key_pop(s);
    }

    if ((s->consumer_count || s->pointer_count) && port_ready(port, KVM_USB_ITF_POINTER)) {
        bool consumer = s->consumer_count && (!s->pointer_count || s->consumer_next);
        /* The other stream is preferred next, so the shared endpoint never starves one. */
        s->consumer_next = !consumer;
        if (consumer) {
            uint8_t report[KVM_USB_HID_CONSUMER_REPORT_LEN];
            kvm_usb_hid_consumer_report(s, report);
            if (port->send(port->context, KVM_USB_ITF_POINTER, KVM_USB_REPORT_ID_CONSUMER, report, sizeof(report)))
                consumer_pop(s);
        } else {
            uint8_t report[KVM_USB_HID_POINTER_REPORT_LEN];
            int32_t chunk[4];
            if (pointer_chunk(s, report, chunk) &&
                port->send(port->context, KVM_USB_ITF_POINTER, KVM_USB_REPORT_ID_MOUSE, report, sizeof(report)))
                pointer_commit(s, chunk);
        }
    }
    return !kvm_usb_hid_sink_pending(s);
}

bool kvm_usb_hid_sink_pending(const kvm_usb_hid_sink_t *s)
{
    if (s == NULL) return false;
    return s->key_count != 0 || s->pointer_count != 0 || s->consumer_count != 0;
}
