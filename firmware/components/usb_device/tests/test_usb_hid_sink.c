/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Proves the ordered USB HID sink: report bytes and layout (checked against
 * the Task 2 pointer descriptor's bit totals), per-stream ordering of distinct
 * states (press and release both delivered, in order, even when applied back
 * to back while the endpoint is busy), FIFO overflow that never reorders or
 * leaves a key down, saturating motion and clamped chunking, per-interface
 * pump limits, consumer/pointer alternation, retries of the same entry after a
 * busy or refused send, and the release_all / abandon transitions. */
#include "usb_hid_sink.h"
#include "usb_descriptors.h"
#include <assert.h>
#include <string.h>

#define MAX_CALLS 256

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
    call_t attempts[MAX_CALLS]; /* every send attempt, accepted or not */
    int attempt_count;
} fake_t;

static bool fake_ready(void *context, uint8_t itf)
{
    fake_t *f = context;
    assert(itf == KVM_USB_ITF_KEYBOARD || itf == KVM_USB_ITF_POINTER);
    f->ready_calls[itf]++;
    return f->ready[itf];
}

static void record(call_t *c, uint8_t itf, uint8_t report_id, const uint8_t *data, size_t len)
{
    c->itf = itf;
    c->report_id = report_id;
    c->len = len;
    memset(c->data, 0, sizeof(c->data));
    memcpy(c->data, data, len);
}

static bool fake_send(void *context, uint8_t itf, uint8_t report_id, const uint8_t *data, size_t len)
{
    fake_t *f = context;
    assert(itf == KVM_USB_ITF_KEYBOARD || itf == KVM_USB_ITF_POINTER);
    assert(f->ready[itf]); /* never sent on a busy interface */
    assert(data != NULL && len <= 8);
    f->send_attempts[itf]++;
    assert(f->attempt_count < MAX_CALLS);
    record(&f->attempts[f->attempt_count++], itf, report_id, data, len);
    if (!f->accept[itf]) return false;
    assert(f->count < MAX_CALLS);
    record(&f->calls[f->count++], itf, report_id, data, len);
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

static void apply(kvm_usb_hid_sink_t *s, kvm_router_input_t in) { kvm_usb_hid_sink_apply(s, &in); }

static int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

static bool is_mouse(const call_t *c) { return c->itf == KVM_USB_ITF_POINTER && c->report_id == KVM_USB_REPORT_ID_MOUSE; }
static bool is_consumer(const call_t *c) { return c->itf == KVM_USB_ITF_POINTER && c->report_id == KVM_USB_REPORT_ID_CONSUMER; }
static bool is_keyboard(const call_t *c) { return c->itf == KVM_USB_ITF_KEYBOARD; }

static void assert_mouse(const call_t *c, uint8_t buttons, int x, int y, int wheel, int pan)
{
    assert(is_mouse(c));
    assert(c->len == KVM_USB_HID_POINTER_REPORT_LEN);
    assert(c->data[0] == buttons);
    assert(rd16(c->data + 1) == x);
    assert(rd16(c->data + 3) == y);
    assert((int8_t)c->data[5] == wheel);
    assert((int8_t)c->data[6] == pan);
}

static void assert_consumer(const call_t *c, uint16_t usage)
{
    assert(is_consumer(c));
    assert(c->len == KVM_USB_HID_CONSUMER_REPORT_LEN);
    assert(c->data[0] == (usage & 0xFF) && c->data[1] == (usage >> 8));
}

static void assert_keyboard(const call_t *c, const uint8_t expect[8])
{
    assert(is_keyboard(c));
    assert(c->report_id == 0);
    assert(c->len == KVM_USB_HID_KEYBOARD_REPORT_LEN);
    assert(memcmp(c->data, expect, 8) == 0);
}

/* Collects the n-th report of one stream from the accepted calls. */
static const call_t *nth(const fake_t *f, bool (*kind)(const call_t *), int n)
{
    for (int i = 0; i < f->count; i++)
        if (kind(&f->calls[i]) && n-- == 0) return &f->calls[i];
    return NULL;
}

static int count_of(const fake_t *f, bool (*kind)(const call_t *))
{
    int n = 0;
    for (int i = 0; i < f->count; i++) n += kind(&f->calls[i]) ? 1 : 0;
    return n;
}

static void drain(kvm_usb_hid_sink_t *s, const kvm_usb_hid_port_t *p)
{
    int pumps = 0;
    while (!kvm_usb_hid_sink_pump(s, p)) assert(++pumps < 200);
}

static void assert_idle(const kvm_usb_hid_sink_t *s)
{
    assert(s->key_count == 0 && s->pointer_count == 0 && s->consumer_count == 0);
    assert(!kvm_usb_hid_sink_pending(s));
}

static const uint8_t ZERO8[8] = { 0 };

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
    assert_idle(&s);
    assert(s.buttons == 0 && s.consumer == 0 && memcmp(s.keys, ZERO8, 8) == 0);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.attempt_count == 0);
    uint8_t out[7];
    assert(!kvm_usb_hid_pointer_report(&s, out));
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
    assert(s.key_count == 1 && s.pointer_count == 0 && s.consumer_count == 0);

    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_keyboard(&f.calls[0], bytes);
    assert_idle(&s);
    kvm_usb_hid_keyboard_report(&s, out); /* empty queue: the newest state */
    assert(memcmp(out, bytes, 8) == 0);
}

static void test_equal_states_are_not_queued(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, kbd(0, 0, 0));       /* equals the initial all-up */
    apply(&s, ptr(0, 0, 0, 0, 0)); /* no change, no motion */
    apply(&s, cons(0));
    assert_idle(&s);
    apply(&s, kbd(0, 4, 0));
    apply(&s, kbd(0, 4, 0));
    apply(&s, cons(0xE9));
    apply(&s, cons(0xE9));
    apply(&s, ptr(1, 0, 0, 0, 0));
    apply(&s, ptr(1, 0, 0, 0, 0));
    assert(s.key_count == 1 && s.consumer_count == 1 && s.pointer_count == 1);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    drain(&s, &p);
    apply(&s, kbd(0, 4, 0)); /* equals the last sent state */
    apply(&s, cons(0xE9));
    apply(&s, ptr(1, 0, 0, 0, 0));
    assert_idle(&s);
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
    kvm_usb_hid_sink_release_all(&s); /* a real all-up after a phantom report */
    kvm_usb_hid_keyboard_report(&s, out);
    assert(memcmp(out, ZERO8, 8) == 0);
}

static void test_press_release_back_to_back_while_busy(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    /* One BLE connection event delivers press and release together; the endpoints are busy. */
    apply(&s, kbd(0, 0x04, 0));
    apply(&s, kbd(0, 0, 0));
    apply(&s, ptr(1, 0, 0, 0, 0));
    apply(&s, ptr(0, 0, 0, 0, 0));
    f.ready[0] = f.ready[1] = false;
    for (int i = 0; i < 3; i++) assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.attempt_count == 0);
    f.ready[0] = f.ready[1] = true;
    drain(&s, &p);
    const uint8_t a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    assert(count_of(&f, is_keyboard) == 2 && count_of(&f, is_mouse) == 2);
    assert_keyboard(nth(&f, is_keyboard, 0), a);
    assert_keyboard(nth(&f, is_keyboard, 1), ZERO8);
    assert_mouse(nth(&f, is_mouse, 0), 1, 0, 0, 0, 0);
    assert_mouse(nth(&f, is_mouse, 1), 0, 0, 0, 0, 0);
    assert_idle(&s);
}

static void test_rapid_pairs_overflow_keeps_order_and_ends_up(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    f.ready[0] = false;
    for (int i = 0; i < 20; i++) {
        apply(&s, kbd(0, (uint8_t)(0x04 + i), 0));
        apply(&s, kbd(0, 0, 0));
        assert(s.key_count <= KVM_USB_HID_KEY_QUEUE_DEPTH);
    }
    assert(s.key_count == KVM_USB_HID_KEY_QUEUE_DEPTH);
    f.ready[0] = true;
    drain(&s, &p);
    int n = count_of(&f, is_keyboard);
    assert(n == (int)KVM_USB_HID_KEY_QUEUE_DEPTH);
    uint8_t last_key = 0;
    for (int i = 0; i < n; i++) {
        const call_t *c = nth(&f, is_keyboard, i);
        if (i % 2 == 1) assert_keyboard(c, ZERO8); /* press, release, press, release ... */
        else {
            assert(c->data[2] > last_key); /* presses keep their original order */
            last_key = c->data[2];
        }
    }
    assert_keyboard(nth(&f, is_keyboard, n - 1), ZERO8); /* never ends with a key down */
    /* Once full, each later press was replaced by its own release in the newest slot:
     * the first eight pairs arrive intact, the rest merge into the final release. */
    assert(last_key == 0x04 + 7);
    assert_idle(&s);

    /* Same for buttons and consumer. */
    fake_init(&f);
    f.ready[1] = false;
    for (int i = 0; i < 20; i++) {
        apply(&s, ptr(1, 1, 0, 0, 0));
        apply(&s, ptr(0, 0, 0, 0, 0));
        apply(&s, cons((uint16_t)(0xB0 + i)));
        apply(&s, cons(0));
    }
    assert(s.pointer_count == KVM_USB_HID_POINTER_QUEUE_DEPTH && s.consumer_count == KVM_USB_HID_CONSUMER_QUEUE_DEPTH);
    f.ready[1] = true;
    drain(&s, &p);
    int mice = count_of(&f, is_mouse), consumers = count_of(&f, is_consumer);
    assert(mice == (int)KVM_USB_HID_POINTER_QUEUE_DEPTH && consumers == (int)KVM_USB_HID_CONSUMER_QUEUE_DEPTH);
    for (int i = 0; i < mice; i++) assert(nth(&f, is_mouse, i)->data[0] == (i % 2 == 0 ? 1 : 0));
    int total_dx = 0;
    for (int i = 0; i < mice; i++) total_dx += rd16(nth(&f, is_mouse, i)->data + 1);
    assert(total_dx == 20); /* merging never loses motion */
    uint16_t prev = 0;
    for (int i = 0; i < consumers; i++) {
        const call_t *c = nth(&f, is_consumer, i);
        uint16_t u = (uint16_t)(c->data[0] | (c->data[1] << 8));
        if (i % 2 == 1) assert(u == 0);
        else { assert(u > prev); prev = u; }
    }
    assert_consumer(nth(&f, is_consumer, consumers - 1), 0);
    assert_idle(&s);
}

static void test_click_is_three_ordered_reports(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, ptr(1, 0, 0, 0, 0));
    apply(&s, ptr(1, 4, -2, 0, 0));
    apply(&s, ptr(1, 6, -3, 1, 0));
    apply(&s, ptr(0, 0, 0, 0, 0));
    assert(s.pointer_count == 3);
    drain(&s, &p);
    assert(f.count == 3);
    assert_mouse(&f.calls[0], 1, 0, 0, 0, 0);    /* press */
    assert_mouse(&f.calls[1], 1, 10, -5, 1, 0);  /* motion while held */
    assert_mouse(&f.calls[2], 0, 0, 0, 0, 0);    /* release */
}

static void test_transition_carries_its_own_motion(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, ptr(0, 3, 3, 0, 0));  /* motion before the press */
    apply(&s, ptr(1, 2, 0, 0, 0));  /* press report with motion */
    apply(&s, ptr(1, 5, 0, 0, 0));
    apply(&s, ptr(0, 7, 0, 0, 0));  /* release report with motion */
    drain(&s, &p);
    assert(f.count == 4);
    assert_mouse(&f.calls[0], 0, 3, 3, 0, 0);
    assert_mouse(&f.calls[1], 1, 2, 0, 0, 0);
    assert_mouse(&f.calls[2], 1, 5, 0, 0, 0);
    assert_mouse(&f.calls[3], 0, 7, 0, 0, 0);
}

static void test_pointer_accumulates_three_applies_one_report(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, ptr(0, 10, -5, 1, 0));
    apply(&s, ptr(0, 20, -6, 2, -1));
    apply(&s, ptr(0, -3, 1, 0, -2));
    assert(s.pointer_count == 1);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_mouse(&f.calls[0], 0, 27, -10, 3, -3);
    assert_idle(&s);
}

static void test_motion_that_cancels_out_is_dropped(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, ptr(0, 5, 0, 0, 0));
    apply(&s, ptr(0, -5, 0, 0, 0));
    assert_idle(&s);
}

static void test_pointer_report_byte_layout(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, ptr(0xFF, 0x1234, -2, -128, 127));
    assert(s.buttons == 0x1F); /* padding bits masked on apply */
    uint8_t out[7];
    memset(out, 0xEE, sizeof(out));
    assert(kvm_usb_hid_pointer_report(&s, out));
    const uint8_t expect[7] = { 0x1F, 0x34, 0x12, 0xFE, 0xFF, 0x81, 0x7F };
    assert(memcmp(out, expect, 7) == 0); /* wheel -128 clamped to -127 */
    assert(s.pointer_count == 1);        /* the wheel remainder is still queued */
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(out[0] == 0x1F && rd16(out + 1) == 0 && rd16(out + 3) == 0 && (int8_t)out[5] == -1 && out[6] == 0);
    assert_idle(&s);
    memset(out, 0xEE, sizeof(out));
    assert(!kvm_usb_hid_pointer_report(&s, out));
    for (int i = 0; i < 7; i++) assert(out[i] == 0);
    apply(&s, ptr(0x3F, 0, 0, 0, 0)); /* a change in padding bits only is no change */
    assert_idle(&s);
}

static void test_clamp_40000_over_two_pumps(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, ptr(0, 20000, -20000, 0, 0));
    apply(&s, ptr(0, 20000, -20000, 0, 0));
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_mouse(&f.calls[0], 0, 32767, -32767, 0, 0);
    assert(kvm_usb_hid_sink_pending(&s));
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_mouse(&f.calls[1], 0, 7233, -7233, 0, 0);
    assert_idle(&s);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
}

static void test_int16_min_delta_is_clamped(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    apply(&s, ptr(0, INT16_MIN, INT16_MIN, 0, 0));
    uint8_t out[7];
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(rd16(out + 1) == -32767 && rd16(out + 3) == -32767);
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(rd16(out + 1) == -1 && rd16(out + 3) == -1);
    assert_idle(&s);
}

static void test_wheel_saturation(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    for (int i = 0; i < 3; i++) apply(&s, ptr(0, 0, 0, 127, -128));
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    drain(&s, &p);
    assert(f.count == 4);
    assert_mouse(&f.calls[0], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[1], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[2], 0, 0, 0, 127, -127);
    assert_mouse(&f.calls[3], 0, 0, 0, 0, -3);

    /* The int16 accumulators saturate instead of wrapping. */
    kvm_usb_hid_sink_init(&s);
    for (int i = 0; i < 300; i++) apply(&s, ptr(0, 0, 0, 127, -128));
    assert(s.pointer_count == 1);
    const kvm_usb_hid_pointer_entry_t *e = &s.pointer_queue[s.pointer_head];
    assert(e->wheel == INT16_MAX && e->pan == INT16_MIN);
    apply(&s, ptr(0, 0, 0, -1, 1));
    assert(e->wheel == INT16_MAX - 1 && e->pan == INT16_MIN + 1);
}

static void test_motion_accumulators_saturate(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    for (int i = 0; i < 70000; i++) apply(&s, ptr(0, 32767, -32768, 0, 0));
    assert(s.pointer_count == 1);
    const kvm_usb_hid_pointer_entry_t *e = &s.pointer_queue[s.pointer_head];
    assert(e->dx == INT32_MAX && e->dy == INT32_MIN);
    uint8_t out[7];
    assert(kvm_usb_hid_pointer_report(&s, out));
    assert(rd16(out + 1) == 32767 && rd16(out + 3) == -32767);
    assert(e->dx == INT32_MAX - 32767 && e->dy == INT32_MIN + 32767);
    apply(&s, ptr(0, -5, 5, 0, 0));
    assert(e->dx == INT32_MAX - 32772 && e->dy == INT32_MIN + 32772);
}

static void test_consumer_press_then_release_with_busy_first_pump(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, cons(0x00E9));
    f.ready[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.attempt_count == 0 && f.ready_calls[1] == 1);
    f.ready[1] = true;
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1);
    assert_consumer(&f.calls[0], 0x00E9);
    apply(&s, cons(0));
    f.ready[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    f.ready[1] = true;
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert_consumer(&f.calls[1], 0);
    assert_idle(&s);
}

static void test_consumer_tap_while_busy_delivers_both(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    f.ready[1] = false;
    apply(&s, cons(0x00CD));
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    apply(&s, cons(0)); /* release applied while the endpoint is busy */
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    f.ready[1] = true;
    drain(&s, &p);
    assert(f.count == 2);
    assert_consumer(&f.calls[0], 0x00CD);
    assert_consumer(&f.calls[1], 0);
}

static void test_alternation_pointer_and_consumer(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, ptr(0, 5, 6, 0, 0));
    apply(&s, cons(0x00B5));
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1); /* one report per interface per call */
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2);
    assert(count_of(&f, is_mouse) == 1 && count_of(&f, is_consumer) == 1);
    assert_mouse(nth(&f, is_mouse, 0), 0, 5, 6, 0, 0);
    assert_consumer(nth(&f, is_consumer, 0), 0x00B5);
    assert_idle(&s);
}

static void test_alternation_under_continuous_motion(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    /* Motion arrives before every pump; consumer press and release must still get through. */
    apply(&s, cons(0x00E9));
    for (int i = 0; i < 6; i++) {
        apply(&s, ptr(0, 1, 0, 0, 0));
        if (i == 2) apply(&s, cons(0));
        kvm_usb_hid_sink_pump(&s, &p);
    }
    assert(count_of(&f, is_consumer) == 2);
    assert_consumer(nth(&f, is_consumer, 0), 0x00E9);
    assert_consumer(nth(&f, is_consumer, 1), 0);
    assert(count_of(&f, is_mouse) >= 3);
    drain(&s, &p);
    int total_dx = 0;
    for (int i = 0; i < count_of(&f, is_mouse); i++) total_dx += rd16(nth(&f, is_mouse, i)->data + 1);
    assert(total_dx == 6); /* no motion lost or duplicated */
}

static void test_one_report_per_interface_per_call(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, kbd(0, 0x04, 0));
    apply(&s, kbd(0, 0x04, 0x05));
    apply(&s, ptr(1, 1, 1, 0, 0));
    apply(&s, cons(0x00E2));
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 2 && f.send_attempts[0] == 1 && f.send_attempts[1] == 1);
    assert(is_keyboard(&f.calls[0]) && f.calls[1].itf == KVM_USB_ITF_POINTER);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 4 && is_keyboard(&f.calls[2]) && f.calls[3].itf == KVM_USB_ITF_POINTER);
}

static void test_busy_ready_leaves_entries_queued(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    f.ready[0] = f.ready[1] = false;
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, kbd(0x01, 0x04, 0));
    apply(&s, ptr(1, 9, 0, 0, 0));
    apply(&s, cons(0x00E2));
    for (int i = 0; i < 3; i++) assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.attempt_count == 0);
    assert(s.key_count == 1 && s.pointer_count == 1 && s.consumer_count == 1);
    f.ready[0] = true; /* keyboard alone becomes ready */
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1 && is_keyboard(&f.calls[0]));
    assert(s.key_count == 0 && s.pointer_count == 1 && s.consumer_count == 1);
}

static void test_send_false_retries_the_same_entry(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    f.accept[0] = f.accept[1] = false;
    kvm_usb_hid_port_t p = port_of(&f);
    const uint8_t a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    apply(&s, kbd(0, 0x04, 0));
    apply(&s, kbd(0, 0, 0));
    apply(&s, ptr(2, 20000, 0, 3, 0));
    apply(&s, ptr(2, 20000, 0, 3, 0));
    apply(&s, ptr(0, 0, 0, 0, 0));
    for (int i = 0; i < 4; i++) assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 0 && f.send_attempts[0] == 4 && f.send_attempts[1] == 4);
    for (int i = 0; i < f.attempt_count; i++) {
        const call_t *c = &f.attempts[i];
        if (is_keyboard(c)) assert_keyboard(c, a);            /* always the press, never skipped */
        else assert_mouse(c, 2, 20000, 0, 3, 0);              /* always the press transition */
    }
    assert(s.key_count == 2 && s.pointer_count == 3);
    f.accept[0] = f.accept[1] = true;
    drain(&s, &p);
    assert(count_of(&f, is_keyboard) == 2);
    assert_keyboard(nth(&f, is_keyboard, 0), a);
    assert_keyboard(nth(&f, is_keyboard, 1), ZERO8);
    assert(count_of(&f, is_mouse) == 3);
    assert_mouse(nth(&f, is_mouse, 0), 2, 20000, 0, 3, 0);
    assert_mouse(nth(&f, is_mouse, 1), 2, 20000, 0, 3, 0);
    assert_mouse(nth(&f, is_mouse, 2), 0, 0, 0, 0, 0);
    assert_idle(&s);
}

static void test_refused_chunk_is_not_consumed(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, ptr(0, 20000, 0, 0, 0));
    apply(&s, ptr(0, 20000, 0, 0, 0));
    f.accept[1] = false;
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    assert(s.pointer_queue[s.pointer_head].dx == 40000);
    f.accept[1] = true;
    drain(&s, &p);
    assert(f.count == 2);
    assert_mouse(&f.calls[0], 0, 32767, 0, 0, 0);
    assert_mouse(&f.calls[1], 0, 7233, 0, 0, 0);
}

static void test_drag_release_keeps_motion_under_held_button(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, ptr(1, 20000, 0, 0, 0));
    apply(&s, ptr(1, 20000, 0, 0, 0));
    apply(&s, ptr(1, 20000, 0, 0, 0));
    apply(&s, ptr(0, 0, 0, 0, 0));
    drain(&s, &p);
    assert(f.count == 4);
    assert_mouse(&f.calls[0], 1, 20000, 0, 0, 0); /* press with its own motion */
    assert_mouse(&f.calls[1], 1, 32767, 0, 0, 0); /* held motion, chunked */
    assert_mouse(&f.calls[2], 1, 7233, 0, 0, 0);
    assert_mouse(&f.calls[3], 0, 0, 0, 0, 0);     /* release after every held chunk */
}

static void test_release_all_sends_only_the_zero_state(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    f.ready[0] = f.ready[1] = false;
    apply(&s, kbd(0x02, 0x04, 0));
    apply(&s, kbd(0x02, 0x04, 0x05));
    apply(&s, ptr(3, 30000, -30000, 5, 5));
    apply(&s, ptr(1, 30000, -30000, 5, 5));
    apply(&s, cons(0x00E9));
    apply(&s, cons(0x00EA));
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_release_all(&s);
    assert(s.key_count == 1 && s.pointer_count == 1 && s.consumer_count == 1);
    assert(s.buttons == 0 && s.consumer == 0 && memcmp(s.keys, ZERO8, 8) == 0);
    f.ready[0] = f.ready[1] = true;
    drain(&s, &p);
    assert(f.count == 3);
    assert_keyboard(nth(&f, is_keyboard, 0), ZERO8);
    assert_mouse(nth(&f, is_mouse, 0), 0, 0, 0, 0, 0);
    assert_consumer(nth(&f, is_consumer, 0), 0);
    assert_idle(&s);
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
        drain(&s, &p);
        assert(f.count == 3 * round);
    }
}

static void test_release_all_apply_interleavings(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    const uint8_t one[8] = { 0, 0, 0x1E, 0, 0, 0, 0, 0 };
    /* Queued input before release_all is dropped; input after it follows the zero state. */
    apply(&s, ptr(1, 100, 100, 1, 1));
    kvm_usb_hid_sink_release_all(&s);
    apply(&s, ptr(0, 7, -7, 0, 0));
    apply(&s, kbd(0, 0x1E, 0));
    drain(&s, &p);
    assert(f.count == 5);
    assert_keyboard(nth(&f, is_keyboard, 0), ZERO8);
    assert_keyboard(nth(&f, is_keyboard, 1), one);
    assert_mouse(nth(&f, is_mouse, 0), 0, 0, 0, 0, 0);
    assert_mouse(nth(&f, is_mouse, 1), 0, 7, -7, 0, 0);
    assert_consumer(nth(&f, is_consumer, 0), 0);

    /* release_all, apply, release_all again before any pump: only the zero states remain. */
    fake_init(&f);
    kvm_usb_hid_sink_release_all(&s);
    apply(&s, kbd(0x01, 0x04, 0x05));
    apply(&s, cons(0x00E9));
    apply(&s, ptr(2, 50, 0, 0, 0));
    kvm_usb_hid_sink_release_all(&s);
    drain(&s, &p);
    assert(f.count == 3);
    assert_keyboard(nth(&f, is_keyboard, 0), ZERO8);
    assert_mouse(nth(&f, is_mouse, 0), 0, 0, 0, 0, 0);
    assert_consumer(nth(&f, is_consumer, 0), 0);

    /* A pump between release_all and the next apply delivers the all-up first. */
    fake_init(&f);
    const uint8_t k2[8] = { 0x01, 0, 0x04, 0x05, 0, 0, 0, 0 };
    apply(&s, kbd(0x01, 0x04, 0x05));
    assert(kvm_usb_hid_sink_pump(&s, &p));
    kvm_usb_hid_sink_release_all(&s);
    assert(!kvm_usb_hid_sink_pump(&s, &p));
    apply(&s, kbd(0, 0x1E, 0));
    drain(&s, &p);
    assert(count_of(&f, is_keyboard) == 3);
    assert_keyboard(nth(&f, is_keyboard, 0), k2);
    assert_keyboard(nth(&f, is_keyboard, 1), ZERO8);
    assert_keyboard(nth(&f, is_keyboard, 2), one);
}

static void test_abandon_sends_nothing(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    apply(&s, kbd(0x02, 0x04, 0));
    apply(&s, ptr(1, 20000, 0, 1, 1));
    apply(&s, ptr(1, 20000, 0, 1, 1));
    apply(&s, cons(0x00E9));
    kvm_usb_hid_sink_release_all(&s);
    kvm_usb_hid_sink_abandon(&s);
    assert_idle(&s);
    assert(s.buttons == 0 && s.consumer == 0 && memcmp(s.keys, ZERO8, 8) == 0);
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.attempt_count == 0);
    apply(&s, kbd(0x02, 0x04, 0)); /* after abandon, new input is delivered normally */
    assert(kvm_usb_hid_sink_pump(&s, &p));
    assert(f.count == 1 && is_keyboard(&f.calls[0]));
}

static void test_pending_reflects_queues(void)
{
    kvm_usb_hid_sink_t s;
    kvm_usb_hid_sink_init(&s);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t p = port_of(&f);
    assert(!kvm_usb_hid_sink_pending(&s));
    apply(&s, kbd(0, 4, 0));
    assert(kvm_usb_hid_sink_pending(&s));
    drain(&s, &p);
    apply(&s, ptr(0, 0, 0, 0, 1));
    assert(kvm_usb_hid_sink_pending(&s));
    drain(&s, &p);
    apply(&s, cons(1));
    assert(kvm_usb_hid_sink_pending(&s));
    drain(&s, &p);
    apply(&s, ptr(3, 0, 0, 0, 0));
    drain(&s, &p);
    /* Held state alone owes nothing. */
    assert(s.buttons == 3 && s.consumer == 1 && s.keys[2] == 4);
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
    assert_idle(&s);
    assert(!kvm_usb_hid_sink_pending(NULL));
    kvm_usb_hid_sink_release_all(NULL);
    kvm_usb_hid_sink_abandon(NULL);
    kvm_usb_hid_sink_init(NULL);

    apply(&s, kbd(0, 4, 0));
    assert(!kvm_usb_hid_sink_pump(&s, NULL));
    kvm_usb_hid_port_t no_send = { NULL, NULL, NULL };
    assert(!kvm_usb_hid_sink_pump(&s, &no_send));
    assert(s.key_count == 1);
    fake_t f;
    fake_init(&f);
    kvm_usb_hid_port_t no_ready = { NULL, fake_send, &f }; /* NULL ready means always ready */
    assert(kvm_usb_hid_sink_pump(&s, &no_ready));
    assert(f.count == 1);
    assert(kvm_usb_hid_sink_pump(NULL, &no_ready));
}

int main(void)
{
    test_layout_matches_descriptors();
    test_init_is_idle();
    test_keyboard_bytes_equal_input();
    test_equal_states_are_not_queued();
    test_keyboard_rollover_passes_through();
    test_press_release_back_to_back_while_busy();
    test_rapid_pairs_overflow_keeps_order_and_ends_up();
    test_click_is_three_ordered_reports();
    test_transition_carries_its_own_motion();
    test_pointer_accumulates_three_applies_one_report();
    test_motion_that_cancels_out_is_dropped();
    test_pointer_report_byte_layout();
    test_clamp_40000_over_two_pumps();
    test_int16_min_delta_is_clamped();
    test_wheel_saturation();
    test_motion_accumulators_saturate();
    test_consumer_press_then_release_with_busy_first_pump();
    test_consumer_tap_while_busy_delivers_both();
    test_alternation_pointer_and_consumer();
    test_alternation_under_continuous_motion();
    test_one_report_per_interface_per_call();
    test_busy_ready_leaves_entries_queued();
    test_send_false_retries_the_same_entry();
    test_refused_chunk_is_not_consumed();
    test_drag_release_keeps_motion_under_held_button();
    test_release_all_sends_only_the_zero_state();
    test_release_all_resends_when_already_zero();
    test_release_all_apply_interleavings();
    test_abandon_sends_nothing();
    test_pending_reflects_queues();
    test_null_and_unknown_inputs();
    return 0;
}
