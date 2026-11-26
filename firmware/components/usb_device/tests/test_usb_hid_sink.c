/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Proves the state-based USB HID sink: report bytes and layout (checked
 * against the Task 2 pointer descriptor's bit totals), saturating motion
 * accumulation and clamped chunking, per-interface pump limits, consumer and
 * pointer alternation, dirty flags surviving busy or refused sends, and the
 * release_all / abandon transitions that keep the PC from holding a key. */
#include "usb_hid_sink.h"
#include "usb_descriptors.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

#define MAX_CALLS 64

typedef struct {
    uint8_t itf;
    uint8_t report_id;
    size_t len;
    uint8_t data[8];
} call_t;

typedef struct {
    bool ready[2];
    bool accept[2];
    int ready_calls[2];
    int send_attempts[2];
    int count;
    call_t calls[MAX_CALLS];
} fake_t;

static bool fake_ready(void *context, uint8_t itf)
{
    fake_t *f = context;
    assert(itf == KVM_USB_ITF_KEYBOARD || itf == KVM_USB_ITF_POINTER);
    f->ready_calls[itf]++;
    return f->ready[itf];
}

static bool fake_send(void *context, uint8_t itf, uint8_t report_id, const uint8_t *data, size_t len)
{
    fake_t *f = context;
    assert(itf == KVM_USB_ITF_KEYBOARD || itf == KVM_USB_ITF_POINTER);
    assert(f->ready[itf]); /* never sent on a busy interface */
    assert(data != NULL && len <= 8);
    f->send_attempts[itf]++;
    if (!f->accept[itf]) return false;
    assert(f->count < MAX_CALLS);
    call_t *c = &f->calls[f->count++];
    c->itf = itf;
    c->report_id = report_id;
    c->len = len;
    memset(c->data, 0, sizeof(c->data));
    memcpy(c->data, data, len);
    return true;
}

static void fake_init(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->ready[0] = f->ready[1] = true;
    f->accept[0] = f->accept[1] = true;
}

static kvm_usb_hid_port_t port_of(fake_t *f)
{
    kvm_usb_hid_port_t p = { fake_ready, fake_send, f };
    return p;
}

static kvm_router_input_t kbd(uint8_t mod, uint8_t k0, uint8_t k1)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = KVM_ROUTER_KEYBOARD;
    in.keyboard[0] = mod;
    in.keyboard[2] = k0;
    in.keyboard[3] = k1;
    return in;
}

static kvm_router_input_t ptr(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = KVM_ROUTER_POINTER;
    in.buttons = buttons;
    in.dx = dx;
    in.dy = dy;
    in.wheel = wheel;
    in.pan = pan;
    return in;
}

static kvm_router_input_t cons(uint16_t usage)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = KVM_ROUTER_CONSUMER;
    in.consumer = usage;
    return in;
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

static void assert_mouse(const call_t *c, uint8_t buttons, int x, int y, int wheel, int pan)
{
    assert(c->itf == KVM_USB_ITF_POINTER);
    assert(c->report_id == KVM_USB_REPORT_ID_MOUSE);
    assert(c->len == KVM_USB_HID_POINTER_REPORT_LEN);
    assert(c->data[0] == buttons);
    assert(rd16(c->data + 1) == x);
    assert(rd16(c->data + 3) == y);
    assert((int8_t)c->data[5] == wheel);
    assert((int8_t)c->data[6] == pan);
}

static void assert_consumer(const call_t *c, uint16_t usage)
{
    assert(c->itf == KVM_USB_ITF_POINTER);
    assert(c->report_id == KVM_USB_REPORT_ID_CONSUMER);
    assert(c->len == KVM_USB_HID_CONSUMER_REPORT_LEN);
    assert(c->data[0] == (usage & 0xFF) && c->data[1] == (usage >> 8));
}

static void assert_keyboard(const call_t *c, const uint8_t expect[8])
{
    assert(c->itf == KVM_USB_ITF_KEYBOARD);
    assert(c->report_id == 0);
    assert(c->len == KVM_USB_HID_KEYBOARD_REPORT_LEN);
    assert(memcmp(c->data, expect, 8) == 0);
}

static void assert_nothing_owed(const kvm_usb_hid_sink_t *s)
{
    assert(!s->keys_dirty && !s->pointer_dirty && !s->consumer_dirty);
    assert(s->dx == 0 && s->dy == 0 && s->wheel == 0 && s->pan == 0);
    assert(!kvm_usb_hid_sink_pending(s));
}

/* Minimal HID short-item walker: total Input bits per report ID (as in the descriptor suite). */
static unsigned input_bits(const uint8_t *d, size_t len, uint8_t want_id)
{
    unsigned size = 0, count = 0, total = 0;
    uint8_t id = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t prefix = d[i];
        size_t n = (prefix & 3) == 3 ? 4 : (prefix & 3);
        assert(i + 1 + n <= len);
        uint32_t v = 0;
        for (size_t b = 0; b < n; b++) v |= (uint32_t)d[i + 1 + b] << (8 * b);
        uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1 && tag == 7) size = v;       /* Report Size */
        else if (type == 1 && tag == 9) count = v; /* Report Count */
        else if (type == 1 && tag == 8) id = (uint8_t)v;
        else if (type == 0 && tag == 8 && id == want_id) total += size * count;
        i += 1 + n;
    }
    return total;
}

static void test_layout_matches_descriptors(void)
{
    assert(input_bits(kvm_usb_keyboard_report_desc, kvm_usb_keyboard_report_desc_len, 0) == KVM_USB_HID_KEYBOARD_REPORT_LEN * 8);
    assert(input_bits(kvm_usb_pointer_report_desc, kvm_usb_pointer_report_desc_len, KVM_USB_REPORT_ID_MOUSE) == KVM_USB_HID_POINTER_REPORT_LEN * 8);
    assert(input_bits(kvm_usb_pointer_report_desc, kvm_usb_pointer_report_desc_len, KVM_USB_REPORT_ID_CONSUMER) == KVM_USB_HID_CONSUMER_REPORT_LEN * 8);
}

static void test_init_is_idle(void)
{
    kvm_usb_hid_sink_t s;
    memset(&s, 0xA5, sizeof(s));
    kvm_usb_hid_sink_init(&s);
    assert_nothing_owed(&s);
    assert(s.buttons == 0 && s.consumer == 0);
    for (int i = 0; i < 8; i++) assert(s.keys[i] == 0);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 0 && f.send_attempts[0] == 0 && f.send_attempts[1] == 0);
}

static void test_keyboard_bytes_equal_input(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = KVM_ROUTER_KEYBOARD;
    const uint8_t bytes[8] = { 0x22, 0x5A, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 }; /* reserved byte kept verbatim */
    memcpy(in.keyboard, bytes, 8);
    kvm_usb_hid_sink_apply(&s, &in);
    uint8_t out[8];
    kvm_usb_hid_keyboard_report(&s, out);
    assert(memcmp(out, bytes, 8) == 0);
    assert(s.keys_dirty && !s.pointer_dirty && !s.consumer_dirty);

    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_keyboard(&f.calls[0], bytes);
    assert_nothing_owed(&s);
}

static void test_keyboard_rollover_passes_through(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = KVM_ROUTER_KEYBOARD;
    const uint8_t phantom[8] = { 0x02, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    memcpy(in.keyboard, phantom, 8);
    kvm_usb_hid_sink_apply(&s, &in);
    uint8_t out[8];
    kvm_usb_hid_keyboard_report(&s, out);
    assert(memcmp(out, phantom, 8) == 0);
    /* release_all after a phantom report is a real all-up. */
    kvm_usb_hid_sink_release_all(&s);
    kvm_usb_hid_keyboard_report(&s, out);
    const uint8_t zero[8] = { 0 };
    assert(memcmp(out, zero, 8) == 0);
}

static void test_pointer_accumulates_three_applies_one_report(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t a = ptr(0x01, 10, -5, 1, 0);
    kvm_router_input_t b = ptr(0x01, 20, -6, 2, -1);
    kvm_router_input_t c = ptr(0x03, -3, 1, 0, -2);
    kvm_usb_hid_sink_apply(&s, &a);
    kvm_usb_hid_sink_apply(&s, &b);
    kvm_usb_hid_sink_apply(&s, &c);
    assert(s.dx == 27 && s.dy == -10 && s.wheel == 3 && s.pan == -3 && s.buttons == 0x03);

    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_mouse(&f.calls[0], 0x03, 27, -10, 3, -3);
    assert_nothing_owed(&s);
    assert(s.buttons == 0x03); /* buttons are held state, not consumed */
}

static void test_pointer_report_byte_layout(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t in = ptr(0xFF, 0x1234, -2, -128, 127);
    kvm_usb_hid_sink_apply(&s, &in);
    uint8_t out[7];
    memset(out, 0xEE, sizeof(out));
    assert(kvm_usb_hid_pointer_report(&s, out));
    const uint8_t expect[7] = { 0x1F, 0x34, 0x12, 0xFE, 0xFF, 0x81, 0x7F };
    assert(memcmp(out, expect, 7) == 0); /* padding bits masked; wheel -128 clamped to -127 */
    assert(s.wheel == -1 && s.dx == 0 && s.dy == 0 && s.pan == 0);
    assert(s.pointer_dirty); /* the wheel remainder is still owed */
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(out[0] == 0x1F && rd16(out + 1) == 0 && rd16(out + 3) == 0 && (int8_t)out[5] == -1 && out[6] == 0);
    assert(!s.pointer_dirty);
    memset(out, 0xEE, sizeof(out));
    assert(!kvm_usb_hid_pointer_report(&s, out)); /* nothing owed */
    for (int i = 0; i < 7; i++) assert(out[i] == 0);
    assert(s.buttons == 0xFF);
}

static void test_clamp_40000_over_two_pumps(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t a = ptr(0, 20000, -20000, 0, 0);
    kvm_usb_hid_sink_apply(&s, &a);
    kvm_usb_hid_sink_apply(&s, &a);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(!kvm_usb_hid_sink_pump(&s, &p)); /* remainder pending */
    assert(f.count == 1);
    assert_mouse(&f.calls[0], 0, 32767, -32767, 0, 0);
    assert(s.dx == 7233 && s.dy == -7233 && s.pointer_dirty && kvm_usb_hid_sink_pending(&s));
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_mouse(&f.calls[1], 0, 7233, -7233, 0, 0);
    assert_nothing_owed(&s);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
}

static void test_int16_min_delta_is_clamped(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t a = ptr(0, INT16_MIN, INT16_MIN, 0, 0);
    kvm_usb_hid_sink_apply(&s, &a);
    uint8_t out[7];
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(rd16(out + 1) == -32767 && rd16(out + 3) == -32767);
    assert(s.dx == -1 && s.dy == -1 && s.pointer_dirty);
}

static void test_wheel_saturation(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_router_input_t up = ptr(0, 0, 0, 127, -128);
    for (int i = 0; i < 3; i++) kvm_usb_hid_sink_apply(&s, &up);
    assert(s.wheel == 381 && s.pan == -384);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    int pumps = 0;
    while (!kvm_usb_hid_sink_pump(&s, &p)) assert(++pumps < 10);
    assert(f.count == 4);
    assert_mouse(&f.calls[0], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[1], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[2], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[3], 0, 0, 0, 0, -3);

    /* The int16 accumulators saturate instead of wrapping. */
    kvm_usb_hid_sink_init(&s);
    for (int i = 0; i < 300; i++) kvm_usb_hid_sink_apply(&s, &up);
    assert(s.wheel == INT16_MAX && s.pan == INT16_MIN);
    kvm_router_input_t down = ptr(0, 0, 0, -1, 1);
    kvm_usb_hid_sink_apply(&s, &down);
    assert(s.wheel == INT16_MAX - 1 && s.pan == INT16_MIN + 1);
}

static void test_motion_accumulators_saturate(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    s.dx = INT32_MAX - 10;
    s.dy = INT32_MIN + 10;
    s.pointer_dirty = true;
    kvm_router_input_t big = ptr(0, 32767, -32768, 0, 0);
    kvm_usb_hid_sink_apply(&s, &big);
    kvm_usb_hid_sink_apply(&s, &big);
    assert(s.dx == INT32_MAX && s.dy == INT32_MIN);
    uint8_t out[7];
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(rd16(out + 1) == 32767 && rd16(out + 3) == -32767);
    assert(s.dx == INT32_MAX - 32767 && s.dy == INT32_MIN + 32767);
    kvm_router_input_t back = ptr(0, -5, 5, 0, 0);
    kvm_usb_hid_sink_apply(&s, &back);
    assert(s.dx == INT32_MAX - 32772 && s.dy == INT32_MIN + 32772);
}

static void test_consumer_press_then_release_with_busy_first_pump(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t press = cons(0x00E9), release = cons(0);
    kvm_usb_hid_sink_apply(&s, &press);
    f.ready[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 0 && f.send_attempts[1] == 0 && f.ready_calls[1] == 1);
    assert(s.consumer_dirty);
    f.ready[1] = true;
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_consumer(&f.calls[0], 0x00E9);
    kvm_usb_hid_sink_apply(&s, &release);
    f.ready[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    f.ready[1] = true;
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_consumer(&f.calls[1], 0);
    assert_nothing_owed(&s);
}

static void test_consumer_coalesces_to_latest_state(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t press = cons(0x00CD), release = cons(0);
    kvm_usb_hid_sink_apply(&s, &press);
    f.ready[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_apply(&s, &release); /* applied while the endpoint is busy */
    f.ready[1] = true;
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_consumer(&f.calls[0], 0); /* the release is never lost */
}

static void test_alternation_pointer_and_consumer(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t m = ptr(0, 5, 6, 0, 0), c = cons(0x00B5);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1); /* one report per interface per call */
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    bool saw_mouse = false, saw_consumer = false;
    for (int i = 0; i < 2; i++) {
        if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) { assert_mouse(&f.calls[i], 0, 5, 6, 0, 0); saw_mouse = true; }
        else { assert_consumer(&f.calls[i], 0x00B5); saw_consumer = true; }
    }
    assert(saw_mouse && saw_consumer);
    assert_nothing_owed(&s);
}

static void test_alternation_under_continuous_motion(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t m = ptr(0, 1, 0, 0, 0);
    kvm_router_input_t c1 = cons(0x00E9), c0 = cons(0);
    /* Motion arrives before every pump; consumer press and release must still get through. */
    kvm_usb_hid_sink_apply(&s, &c1);
    for (int i = 0; i < 6; i++) {
        kvm_usb_hid_sink_apply(&s, &m);
        if (i == 2) kvm_usb_hid_sink_apply(&s, &c0);
        kvm_usb_hid_sink_pump(&s, &p);
    }
    int consumers = 0, mice = 0;
    uint16_t last_usage = 0xFFFF;
    for (int i = 0; i < f.count; i++) {
        if (f.calls[i].report_id == KVM_USB_REPORT_ID_CONSUMER) { consumers++; last_usage = (uint16_t)(f.calls[i].data[0] | (f.calls[i].data[1] << 8)); }
        else mice++;
    }
    assert(consumers == 2 && last_usage == 0);
    assert(mice >= 3);
    while (!kvm_usb_hid_sink_pump(&s, &p)) {}
    int total_dx = 0;
    for (int i = 0; i < f.count; i++)
        if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) total_dx += rd16(f.calls[i].data + 1);
    assert(total_dx == 6); /* no motion lost or duplicated */
}

static void test_one_report_per_interface_per_call(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0, 0x04, 0), m = ptr(1, 1, 1, 0, 0), c = cons(0x00E2);
    kvm_usb_hid_sink_apply(&s, &k);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert(f.calls[0].itf == KVM_USB_ITF_KEYBOARD);
    assert(f.calls[1].itf == KVM_USB_ITF_POINTER);
    assert(f.send_attempts[0] == 1 && f.send_attempts[1] == 1);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 3 && f.calls[2].itf == KVM_USB_ITF_POINTER);
}

static void test_busy_ready_leaves_dirty(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    f.ready[0] = f.ready[1] = false;
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0x01, 0x04, 0), m = ptr(1, 9, 0, 0, 0), c = cons(0x00E2);
    kvm_usb_hid_sink_apply(&s, &k);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    for (int i = 0; i < 3; i++) assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 0 && f.send_attempts[0] == 0 && f.send_attempts[1] == 0);
    assert(s.keys_dirty && s.pointer_dirty && s.consumer_dirty && s.dx == 9);
    /* Keyboard becomes ready alone: only interface 0 sends. */
    f.ready[0] = true;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1 && f.calls[0].itf == KVM_USB_ITF_KEYBOARD);
    assert(!s.keys_dirty && s.pointer_dirty && s.consumer_dirty);
}

static void test_send_false_leaves_dirty_and_motion(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    f.accept[0] = f.accept[1] = false;
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0, 0x04, 0x05), m = ptr(2, 40000 / 2, 0, 3, 0), c = cons(0x00E9);
    kvm_usb_hid_sink_apply(&s, &k);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.send_attempts[0] == 2 && f.send_attempts[1] == 2 && f.count == 0);
    assert(s.keys_dirty && s.pointer_dirty && s.consumer_dirty);
    assert(s.dx == 40000 && s.wheel == 6); /* a refused chunk is not consumed */
    f.accept[0] = f.accept[1] = true;
    int pumps = 0;
    while (!kvm_usb_hid_sink_pump(&s, &p)) assert(++pumps < 10);
    int total = 0, wheel = 0;
    for (int i = 0; i < f.count; i++)
        if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) { total += rd16(f.calls[i].data + 1); wheel += (int8_t)f.calls[i].data[5]; }
    assert(total == 40000 && wheel == 6);
    assert_nothing_owed(&s);
}

static void test_release_with_remaining_motion_carries_new_buttons(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t drag = ptr(1, 20000, 0, 0, 0);
    kvm_usb_hid_sink_apply(&s, &drag);
    kvm_usb_hid_sink_apply(&s, &drag);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert_mouse(&f.calls[0], 1, 32767, 0, 0, 0);
    kvm_router_input_t up = ptr(0, 0, 0, 0, 0);
    kvm_usb_hid_sink_apply(&s, &up);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_mouse(&f.calls[1], 0, 7233, 0, 0, 0); /* remainder goes out with the release */
    assert_nothing_owed(&s);
}

static void test_button_change_without_motion_is_sent(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t down = ptr(1, 3, 0, 0, 0), up = ptr(0, 0, 0, 0, 0);
    kvm_usb_hid_sink_apply(&s, &down);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_apply(&s, &up);
    assert(s.pointer_dirty && kvm_usb_hid_sink_pending(&s));
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_mouse(&f.calls[1], 0, 0, 0, 0, 0);
}

static void test_release_all_sends_all_up(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0x02, 0x04, 0x05), m = ptr(3, 30000, -30000, 5, 5), c = cons(0x00E9);
    kvm_usb_hid_sink_apply(&s, &k);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    kvm_usb_hid_sink_release_all(&s);
    assert(s.keys_dirty && s.pointer_dirty && s.consumer_dirty);
    assert(s.dx == 0 && s.dy == 0 && s.wheel == 0 && s.pan == 0 && s.buttons == 0 && s.consumer == 0);
    assert(kvm_usb_hid_sink_pending(&s));
    int pumps = 0;
    while (!kvm_usb_hid_sink_pump(&s, &p)) assert(++pumps < 5);
    assert(f.count == 3);
    const uint8_t zero[8] = { 0 };
    int kb = 0, mouse = 0, consumer = 0;
    for (int i = 0; i < f.count; i++) {
        if (f.calls[i].itf == KVM_USB_ITF_KEYBOARD) { assert_keyboard(&f.calls[i], zero); kb++; }
        else if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) { assert_mouse(&f.calls[i], 0, 0, 0, 0, 0); mouse++; }
        else { assert_consumer(&f.calls[i], 0); consumer++; }
    }
    assert(kb == 1 && mouse == 1 && consumer == 1);
    assert_nothing_owed(&s);
}

static void test_release_all_resends_when_already_zero(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    for (int round = 1; round <= 2; round++) {
        kvm_usb_hid_sink_release_all(&s);
        kvm_usb_hid_sink_release_all(&s); /* idempotent: still one report each */
        while (!kvm_usb_hid_sink_pump(&s, &p)) {}
        assert(f.count == 3 * round);
    }
}

static void test_release_all_while_busy_then_ready(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0, 0x04, 0);
    kvm_usb_hid_sink_apply(&s, &k);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    f.ready[0] = f.ready[1] = false;
    kvm_usb_hid_sink_release_all(&s);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    f.ready[0] = f.ready[1] = true;
    while (!kvm_usb_hid_sink_pump(&s, &p)) {}
    const uint8_t zero[8] = { 0 };
    assert(f.count == 4);
    assert_keyboard(&f.calls[1], zero);
}

static void test_release_all_apply_interleavings(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    /* Motion applied before release_all is dropped; motion after it accumulates from zero. */
    kvm_router_input_t m = ptr(1, 100, 100, 1, 1);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_release_all(&s);
    kvm_router_input_t m2 = ptr(0, 7, -7, 0, 0);
    kvm_usb_hid_sink_apply(&s, &m2);
    kvm_router_input_t k = kbd(0, 0x1E, 0);
    kvm_usb_hid_sink_apply(&s, &k); /* new keyboard state after release_all wins */
    while (!kvm_usb_hid_sink_pump(&s, &p)) {}
    const uint8_t one[8] = { 0, 0, 0x1E, 0, 0, 0, 0, 0 };
    int mouse = 0;
    for (int i = 0; i < f.count; i++) {
        if (f.calls[i].itf == KVM_USB_ITF_KEYBOARD) assert_keyboard(&f.calls[i], one);
        else if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) { assert_mouse(&f.calls[i], 0, 7, -7, 0, 0); mouse++; }
        else assert_consumer(&f.calls[i], 0);
    }
    assert(mouse == 1 && f.count == 3);

    /* release_all, apply, release_all again before any pump: all-up is the final state. */
    f.count = 0;
    kvm_usb_hid_sink_release_all(&s);
    kvm_router_input_t k2 = kbd(0x01, 0x04, 0x05), c = cons(0x00E9), m3 = ptr(2, 50, 0, 0, 0);
    kvm_usb_hid_sink_apply(&s, &k2);
    kvm_usb_hid_sink_apply(&s, &c);
    kvm_usb_hid_sink_apply(&s, &m3);
    kvm_usb_hid_sink_release_all(&s);
    while (!kvm_usb_hid_sink_pump(&s, &p)) {}
    const uint8_t zero[8] = { 0 };
    assert(f.count == 3);
    for (int i = 0; i < f.count; i++) {
        if (f.calls[i].itf == KVM_USB_ITF_KEYBOARD) assert_keyboard(&f.calls[i], zero);
        else if (f.calls[i].report_id == KVM_USB_REPORT_ID_MOUSE) assert_mouse(&f.calls[i], 0, 0, 0, 0, 0);
        else assert_consumer(&f.calls[i], 0);
    }

    /* A pump between release_all and the next apply delivers the all-up first. */
    f.count = 0;
    kvm_usb_hid_sink_apply(&s, &k2);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_release_all(&s);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_apply(&s, &k);
    while (!kvm_usb_hid_sink_pump(&s, &p)) {}
    const uint8_t k2_bytes[8] = { 0x01, 0, 0x04, 0x05, 0, 0, 0, 0 };
    int kb = 0;
    const uint8_t *expect[3] = { k2_bytes, zero, one };
    for (int i = 0; i < f.count; i++)
        if (f.calls[i].itf == KVM_USB_ITF_KEYBOARD) { assert(kb < 3); assert_keyboard(&f.calls[i], expect[kb]); kb++; }
    assert(kb == 3);
}

static void test_abandon_sends_nothing(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    kvm_router_input_t k = kbd(0x02, 0x04, 0), m = ptr(1, 40000 / 2, 0, 1, 1), c = cons(0x00E9);
    kvm_usb_hid_sink_apply(&s, &k);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &m);
    kvm_usb_hid_sink_apply(&s, &c);
    kvm_usb_hid_sink_release_all(&s);
    kvm_usb_hid_sink_abandon(&s);
    assert_nothing_owed(&s);
    assert(s.buttons == 0 && s.consumer == 0);
    for (int i = 0; i < 8; i++) assert(s.keys[i] == 0);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 0 && f.send_attempts[0] == 0 && f.send_attempts[1] == 0);
    /* After abandon, new input is delivered normally. */
    kvm_usb_hid_sink_apply(&s, &k);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1 && f.calls[0].itf == KVM_USB_ITF_KEYBOARD);
}

static void test_pending_reflects_flags_and_motion(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    assert(!kvm_usb_hid_sink_pending(&s));
    s.keys_dirty = true;
    assert(kvm_usb_hid_sink_pending(&s));
    s.keys_dirty = false;
    s.pointer_dirty = true;
    assert(kvm_usb_hid_sink_pending(&s));
    s.pointer_dirty = false;
    s.consumer_dirty = true;
    assert(kvm_usb_hid_sink_pending(&s));
    s.consumer_dirty = false;
    s.pan = 1;
    assert(kvm_usb_hid_sink_pending(&s));
    s.pan = 0;
    s.buttons = 3; /* held state alone owes nothing */
    s.consumer = 5;
    s.keys[2] = 4;
    assert(!kvm_usb_hid_sink_pending(&s));
}

static void test_null_and_unknown_inputs(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    kvm_usb_hid_sink_apply(&s, NULL);
    kvm_usb_hid_sink_apply(NULL, NULL);
    kvm_router_input_t bad = kbd(1, 2, 3);
    bad.kind = (kvm_router_input_kind_t)99;
    kvm_usb_hid_sink_apply(&s, &bad);
    assert_nothing_owed(&s);
    assert(!kvm_usb_hid_sink_pending(NULL));
    kvm_usb_hid_sink_release_all(NULL);
    kvm_usb_hid_sink_abandon(NULL);
    kvm_usb_hid_sink_init(NULL);

    kvm_router_input_t k = kbd(0, 4, 0);
    kvm_usb_hid_sink_apply(&s, &k);
    assert(!kvm_usb_hid_sink_pump(&s, NULL));
    kvm_usb_hid_port_t no_send = { NULL, NULL, NULL };
    assert(!kvm_usb_hid_sink_pump(&s, &no_send));
    assert(s.keys_dirty);
    /* NULL ready means always ready. */
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t no_ready = { NULL, fake_send, &f };
    assert(kvm_usb_hid_sink_pump(&s, &no_ready));
    assert(f.count == 1);
    assert(kvm_usb_hid_sink_pump(NULL, &no_ready));
}

int main(void)
{
    test_layout_matches_descriptors();
    test_init_is_idle();
    test_keyboard_bytes_equal_input();
    test_keyboard_rollover_passes_through();
    test_pointer_accumulates_three_applies_one_report();
    test_pointer_report_byte_layout();
    test_clamp_40000_over_two_pumps();
    test_int16_min_delta_is_clamped();
    test_wheel_saturation();
    test_motion_accumulators_saturate();
    test_consumer_press_then_release_with_busy_first_pump();
    test_consumer_coalesces_to_latest_state();
    test_alternation_pointer_and_consumer();
    test_alternation_under_continuous_motion();
    test_one_report_per_interface_per_call();
    test_busy_ready_leaves_dirty();
    test_send_false_leaves_dirty_and_motion();
    test_release_with_remaining_motion_carries_new_buttons();
    test_button_change_without_motion_is_sent();
    test_release_all_sends_all_up();
    test_release_all_resends_when_already_zero();
    test_release_all_while_busy_then_ready();
    test_release_all_apply_interleavings();
    test_abandon_sends_nothing();
    test_pending_reflects_flags_and_motion();
    test_null_and_unknown_inputs();
    return 0;
}
