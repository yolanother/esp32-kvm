/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Proves the composite USB descriptors structurally: device bytes, interface and
 * endpoint budget, IAD placement, HID descriptors, CDC functional descriptors,
 * report layouts, and that the parser never reads out of bounds on truncated
 * input (run under AddressSanitizer with exact-size heap copies). */
#include "usb_descriptors.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Returns the start of the n-th (0-based) descriptor of the given type, or NULL. */
static const uint8_t *find_desc(uint8_t type, int nth)
{
    size_t off = 0;
    while (off + 2 <= kvm_usb_config_descriptor_len) {
        const uint8_t *d = kvm_usb_config_descriptor + off;
        assert(d[0] >= 2 && off + d[0] <= kvm_usb_config_descriptor_len);
        if (d[1] == type && nth-- == 0) return d;
        off += d[0];
    }
    return NULL;
}

/* Returns the interface descriptor for itf number (alt 0). */
static const uint8_t *find_itf(uint8_t number)
{
    for (int i = 0;; i++) {
        const uint8_t *d = find_desc(0x04, i);
        if (d == NULL) return NULL;
        if (d[2] == number) return d;
    }
}

static const uint8_t *find_ep(uint8_t address)
{
    for (int i = 0;; i++) {
        const uint8_t *d = find_desc(0x05, i);
        if (d == NULL) return NULL;
        if (d[2] == address) return d;
    }
}

static void test_device_descriptor(void)
{
    uint8_t d[18];
    memset(d, 0xEE, sizeof(d));
    kvm_usb_device_descriptor(KVM_USB_ESPRESSIF_VID, 0x4009, d);
    const uint8_t expect[18] = { 18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x3A, 0x30, 0x09, 0x40, 0x00, 0x01, 1, 2, 3, 1 };
    assert(memcmp(d, expect, 18) == 0);
    kvm_usb_device_descriptor(0x1234, 0xABCD, d);
    assert(d[8] == 0x34 && d[9] == 0x12 && d[10] == 0xCD && d[11] == 0xAB);
    assert(le16(d + 12) == KVM_USB_BCD_DEVICE);
}

static void test_config_summary(void)
{
    const uint8_t *c = kvm_usb_config_descriptor;
    assert(c[0] == 9 && c[1] == 2);
    assert(le16(c + 2) == kvm_usb_config_descriptor_len);
    assert(c[4] == KVM_USB_ITF_COUNT && c[5] == 1 && c[6] == 0);
    assert(c[7] == 0xA0); /* bus powered + remote wakeup */
    assert(c[8] == 125);  /* 250 mA */

    kvm_usb_config_summary_t s;
    assert(kvm_usb_config_parse(c, kvm_usb_config_descriptor_len, &s));
    assert(s.interfaces == 4 && s.in_endpoints == 4 && s.in_endpoints <= 4 && s.out_endpoints == 1);
    assert(s.iad_before_cdc && s.total_length_ok);
}

static void test_iad_and_cdc(void)
{
    const uint8_t *iad = find_desc(0x0B, 0);
    assert(iad != NULL && iad[0] == 8);
    assert(iad[2] == KVM_USB_ITF_CDC_COMM && iad[3] == 2 && iad[4] == 0x02 && iad[5] == 0x02 && iad[6] == 0x00);
    /* The IAD sits immediately before the CDC communication interface, after interface 1's endpoint. */
    assert(find_desc(0x0B, 1) == NULL);
    assert(iad + iad[0] == find_itf(KVM_USB_ITF_CDC_COMM));
    assert(iad > find_itf(KVM_USB_ITF_POINTER));

    const uint8_t *comm = find_itf(KVM_USB_ITF_CDC_COMM);
    assert(comm[0] == 9 && comm[3] == 0 && comm[4] == 1 && comm[5] == 0x02 && comm[6] == 0x02 && comm[7] == 0x00);
    /* Functional descriptors follow in order: header, call management, ACM, union. */
    const uint8_t *f = comm + comm[0];
    assert(f[0] == 5 && f[1] == 0x24 && f[2] == 0x00 && le16(f + 3) == 0x0110);
    f += f[0];
    assert(f[0] == 5 && f[1] == 0x24 && f[2] == 0x01 && f[3] == 0x00 && f[4] == KVM_USB_ITF_CDC_DATA);
    f += f[0];
    assert(f[0] == 4 && f[1] == 0x24 && f[2] == 0x02 && f[3] == 0x02);
    f += f[0];
    assert(f[0] == 5 && f[1] == 0x24 && f[2] == 0x06 && f[3] == KVM_USB_ITF_CDC_COMM && f[4] == KVM_USB_ITF_CDC_DATA);
    f += f[0];
    assert(f == find_ep(KVM_USB_EP_CDC_NOTIF_IN)); /* notification endpoint closes the comm interface */

    const uint8_t *data = find_itf(KVM_USB_ITF_CDC_DATA);
    assert(data[0] == 9 && data[3] == 0 && data[4] == 2 && data[5] == 0x0A && data[6] == 0x00 && data[7] == 0x00);
}

static void check_ep(uint8_t address, uint8_t attributes, uint16_t max_packet, uint8_t interval_min, uint8_t interval_max)
{
    const uint8_t *e = find_ep(address);
    assert(e != NULL && e[0] == 7 && e[1] == 5);
    assert(e[3] == attributes);
    assert(le16(e + 4) == max_packet);
    assert(e[6] >= interval_min && e[6] <= interval_max);
}

static void test_endpoints(void)
{
    check_ep(KVM_USB_EP_KEYBOARD_IN, 0x03, 8, 1, 1);
    check_ep(KVM_USB_EP_POINTER_IN, 0x03, 16, 1, 1);
    check_ep(KVM_USB_EP_CDC_NOTIF_IN, 0x03, 8, 16, 16);
    check_ep(KVM_USB_EP_CDC_OUT, 0x02, 64, 0, 0);
    check_ep(KVM_USB_EP_CDC_IN, 0x02, 64, 0, 0);
    /* Exactly five endpoint descriptors besides EP0. */
    int total = 0;
    for (int i = 0; find_desc(0x05, i) != NULL; i++) total++;
    assert(total == 5);
}

static void test_hid_interface(uint8_t itf, uint8_t subclass, uint8_t protocol, uint8_t ep, const size_t *report_len)
{
    const uint8_t *i = find_itf(itf);
    assert(i != NULL && i[0] == 9 && i[3] == 0 && i[4] == 1);
    assert(i[5] == 0x03 && i[6] == subclass && i[7] == protocol);
    const uint8_t *h = i + i[0];
    assert(h[0] == 9 && h[1] == 0x21);
    assert(le16(h + 2) == 0x0111);
    assert(h[4] == 0 && h[5] == 1 && h[6] == 0x22); /* bCountryCode 0, one class descriptor: report */
    assert(le16(h + 7) == *report_len);
    assert(h + h[0] == find_ep(ep));
}

/* Minimal HID short-item walker: totals bits per report ID and counts Report ID items. */
typedef struct {
    int ids_declared;
    int id_items;
    unsigned in_bits[4];
    unsigned out_bits[4];
    bool ok;
} hid_walk_t;

static hid_walk_t walk_report(const uint8_t *r, size_t len)
{
    hid_walk_t w;
    memset(&w, 0, sizeof(w));
    w.ok = true;
    unsigned size = 0, count = 0, id = 0;
    size_t off = 0;
    int depth = 0;
    while (off < len) {
        uint8_t prefix = r[off];
        if (prefix == 0xFE) { w.ok = false; return w; }
        size_t data = prefix & 3u;
        if (data == 3) data = 4;
        if (off + 1 + data > len) { w.ok = false; return w; }
        unsigned value = 0;
        for (size_t k = 0; k < data; k++) value |= (unsigned)r[off + 1 + k] << (8 * k);
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            if (tag == 7) size = value;
            else if (tag == 9) count = value;
            else if (tag == 8) { id = value; w.id_items++; assert(id < 4); w.ids_declared |= 1 << id; }
        } else if (type == 0) {
            if (tag == 8) w.in_bits[id] += size * count;
            else if (tag == 9) w.out_bits[id] += size * count;
            else if (tag == 0xA) depth++;
            else if (tag == 0xC) depth--;
        }
        off += 1 + data;
    }
    assert(depth == 0 && off == len);
    return w;
}

static void test_keyboard_report(void)
{
    const uint8_t *r = kvm_usb_keyboard_report_desc;
    assert(kvm_usb_keyboard_report_desc_len == 65);
    assert(r[0] == 0x05 && r[1] == 0x01 && r[2] == 0x09 && r[3] == 0x06 && r[4] == 0xA1 && r[5] == 0x01);
    assert(r[kvm_usb_keyboard_report_desc_len - 1] == 0xC0);
    hid_walk_t w = walk_report(r, kvm_usb_keyboard_report_desc_len);
    assert(w.ok && w.id_items == 0); /* no Report ID: boot keyboard compatible */
    assert(w.in_bits[0] == 64);      /* modifier, reserved, six keys = 8 bytes */
    assert(w.out_bits[0] == 8);      /* five LEDs plus three padding bits */
    /* Modifiers E0..E7 as eight 1-bit fields, the reserved byte, the five LEDs (usage page 8, 1..5) with
     * padding, then the key array, in this order. */
    static const uint8_t mods[] = { 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02 };
    static const uint8_t reserved[] = { 0x95, 0x01, 0x75, 0x08, 0x81, 0x01 };
    static const uint8_t leds[] = { 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01 };
    /* Six 8-bit keys, logical and usage range 0..0xFF as two-byte items so 0xFF is not read as negative. */
    static const uint8_t keys[] = { 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00 };
    const uint8_t *seqs[] = { mods, reserved, leds, keys };
    const size_t lens[] = { sizeof(mods), sizeof(reserved), sizeof(leds), sizeof(keys) };
    size_t cursor = 6;
    for (size_t s = 0; s < 4; s++) {
        bool found = false;
        for (size_t i = cursor; i + lens[s] <= kvm_usb_keyboard_report_desc_len; i++) {
            if (memcmp(r + i, seqs[s], lens[s]) == 0) { found = true; cursor = i + lens[s]; break; }
        }
        assert(found);
    }
    assert(cursor == kvm_usb_keyboard_report_desc_len - 1); /* only the end collection remains */
}

static void test_pointer_report(void)
{
    const uint8_t *r = kvm_usb_pointer_report_desc;
    hid_walk_t w = walk_report(r, kvm_usb_pointer_report_desc_len);
    assert(w.ok);
    assert(w.id_items == 2 && w.ids_declared == ((1 << KVM_USB_REPORT_ID_MOUSE) | (1 << KVM_USB_REPORT_ID_CONSUMER)));
    assert(w.in_bits[KVM_USB_REPORT_ID_MOUSE] == 56);    /* buttons+pad 8, X 16, Y 16, wheel 8, pan 8 */
    assert(w.in_bits[KVM_USB_REPORT_ID_CONSUMER] == 16); /* one 16-bit usage */
    assert(w.out_bits[1] == 0 && w.out_bits[2] == 0);
    assert(r[kvm_usb_pointer_report_desc_len - 1] == 0xC0);

    /* Mouse application collection first, consumer control second. */
    assert(r[0] == 0x05 && r[1] == 0x01 && r[2] == 0x09 && r[3] == 0x02 && r[4] == 0xA1 && r[5] == 0x01 && r[6] == 0x85 && r[7] == KVM_USB_REPORT_ID_MOUSE);
    /* 5 buttons then 3 padding bits (spec Decision 3). */
    static const uint8_t buttons[] = { 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01 };
    /* X/Y: 16-bit, logical -32767..32767, relative. */
    static const uint8_t axes[] = { 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06 };
    /* Wheel: 8-bit relative -127..127. */
    static const uint8_t wheel[] = { 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06 };
    /* AC Pan: consumer page, usage 0x0238. */
    static const uint8_t pan[] = { 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06 };
    static const uint8_t consumer[] = { 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, KVM_USB_REPORT_ID_CONSUMER,
        /* usages and logical range 0..0x3FF in one 16-bit array field */
        0x19, 0x00, 0x2A, 0xFF, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00 };
    const uint8_t *seqs[] = { buttons, axes, wheel, pan, consumer };
    const size_t lens[] = { sizeof(buttons), sizeof(axes), sizeof(wheel), sizeof(pan), sizeof(consumer) };
    size_t cursor = 0;
    for (size_t s = 0; s < 5; s++) {
        bool found = false;
        for (size_t i = cursor; i + lens[s] <= kvm_usb_pointer_report_desc_len; i++) {
            if (memcmp(r + i, seqs[s], lens[s]) == 0) { found = true; cursor = i + lens[s]; break; }
        }
        assert(found); /* each block present, in this order */
    }
}

static void test_hid_interfaces(void)
{
    test_hid_interface(KVM_USB_ITF_KEYBOARD, 0x01, 0x01, KVM_USB_EP_KEYBOARD_IN, &kvm_usb_keyboard_report_desc_len);
    test_hid_interface(KVM_USB_ITF_POINTER, 0x00, 0x00, KVM_USB_EP_POINTER_IN, &kvm_usb_pointer_report_desc_len);
    /* Keyboard is the first interface in the configuration (BIOS looks only there). */
    assert(find_desc(0x04, 0) == find_itf(KVM_USB_ITF_KEYBOARD));
}

static void test_parser_rejects(void)
{
    kvm_usb_config_summary_t s;
    memset(&s, 0x55, sizeof(s));
    assert(!kvm_usb_config_parse(NULL, 10, &s));
    assert(!kvm_usb_config_parse(kvm_usb_config_descriptor, 10, NULL));
    assert(!kvm_usb_config_parse(kvm_usb_config_descriptor, 0, &s));
    assert(s.interfaces == 0 && s.in_endpoints == 0 && !s.iad_before_cdc && !s.total_length_ok);

    size_t n = kvm_usb_config_descriptor_len;
    uint8_t *copy = (uint8_t *)malloc(n);
    assert(copy != NULL);

    /* Zero-length and one-byte descriptors inside the body. */
    for (size_t bad = 0; bad <= 1; bad++) {
        memcpy(copy, kvm_usb_config_descriptor, n);
        copy[9] = (uint8_t)bad; /* first body descriptor (interface 0) */
        assert(!kvm_usb_config_parse(copy, n, &s));
    }
    /* A descriptor claiming to run past the end. */
    memcpy(copy, kvm_usb_config_descriptor, n);
    copy[n - 7] = 0xFF; /* last endpoint's bLength */
    assert(!kvm_usb_config_parse(copy, n, &s));
    /* Wrong header type and wrong header length. */
    memcpy(copy, kvm_usb_config_descriptor, n);
    copy[1] = 0x01;
    assert(!kvm_usb_config_parse(copy, n, &s));
    memcpy(copy, kvm_usb_config_descriptor, n);
    copy[0] = 8;
    assert(!kvm_usb_config_parse(copy, n, &s));
    /* A wTotalLength that disagrees with the buffer: larger fails, smaller leaves trailing bytes. */
    memcpy(copy, kvm_usb_config_descriptor, n);
    copy[2] = (uint8_t)(n + 1);
    assert(!kvm_usb_config_parse(copy, n, &s));
    memcpy(copy, kvm_usb_config_descriptor, n);
    copy[2] = (uint8_t)(n - 7); /* ends on the final endpoint boundary */
    assert(kvm_usb_config_parse(copy, n, &s) && !s.total_length_ok);
    free(copy);

    /* Without the IAD the CDC interface is not flagged. */
    copy = (uint8_t *)malloc(n);
    assert(copy != NULL);
    memcpy(copy, kvm_usb_config_descriptor, n);
    const uint8_t *iad = find_desc(0x0B, 0);
    copy[(size_t)(iad - kvm_usb_config_descriptor) + 1] = 0x20; /* not an IAD any more */
    assert(kvm_usb_config_parse(copy, n, &s) && !s.iad_before_cdc);
    free(copy);
}

/* Every strict prefix of the configuration descriptor, copied to an exact-size heap block so
 * AddressSanitizer flags any read past len, must be rejected by the length check, and the same
 * prefix with a matching wTotalLength must either parse on a descriptor boundary or be rejected. */
static void test_truncations(void)
{
    size_t n = kvm_usb_config_descriptor_len;
    bool boundary[512];
    memset(boundary, 0, sizeof(boundary));
    for (size_t off = 0; off <= n;) {
        boundary[off] = true;
        if (off == n) break;
        off += kvm_usb_config_descriptor[off];
    }
    assert(n < 512);
    for (size_t len = 0; len < n; len++) {
        uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
        assert(copy != NULL);
        if (len) memcpy(copy, kvm_usb_config_descriptor, len);
        kvm_usb_config_summary_t s;
        assert(!kvm_usb_config_parse(copy, len, &s));
        if (len >= 9) {
            copy[2] = (uint8_t)len; /* header agrees with the truncated buffer */
            copy[3] = 0;
            bool ok = kvm_usb_config_parse(copy, len, &s);
            assert(ok == boundary[len]);
            if (ok) assert(s.total_length_ok);
        }
        free(copy);
    }
    /* Zero-filled buffers of any size never parse. */
    for (size_t len = 0; len < 40; len++) {
        uint8_t *zero = (uint8_t *)calloc(len ? len : 1, 1);
        kvm_usb_config_summary_t s;
        assert(zero != NULL && !kvm_usb_config_parse(zero, len, &s));
        free(zero);
    }
}

int main(void)
{
    test_device_descriptor();
    test_config_summary();
    test_iad_and_cdc();
    test_endpoints();
    test_hid_interfaces();
    test_keyboard_report();
    test_pointer_report();
    test_parser_rejects();
    test_truncations();
    return 0;
}
