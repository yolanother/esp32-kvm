/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks the HOGP boot and report-map parsers against real descriptor bytes,
 * including every truncation of each descriptor, run on exact-size heap copies
 * so sanitizers catch any out-of-bounds read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hogp_parse.h"

static int failures;
#define CHECK(c)                                               \
    do {                                                       \
        if (!(c)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            failures++;                                        \
        }                                                      \
    } while (0)

/* Generic wheel mouse: 3 buttons, 5 pad bits, X Y wheel 8-bit relative, no ID. */
static const uint8_t k_wheel_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
/* Boot mouse: 3 buttons, pad, X Y 8-bit relative. */
static const uint8_t k_boot_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
/* Logitech style: report ID 2, 16 buttons, 12-bit X/Y, 8-bit wheel, AC Pan. */
static const uint8_t k_logitech[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07,
    0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x09, 0x38, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0};
/* Boot keyboard only. */
static const uint8_t k_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};
/* Tablet-style absolute X/Y (input flags 0x02). */
static const uint8_t k_absolute[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0, 0xC0};
/* Keyboard (ID 1) followed by wheel mouse (ID 2). */
static const uint8_t k_combo[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85,
    0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75,
    0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15,
    0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};

static uint8_t *exact_copy(const uint8_t *src, size_t len) {
    uint8_t *p = (uint8_t *)malloc(len ? len : 1);
    if (p == NULL) { exit(2); }
    if (len) { memcpy(p, src, len); }
    return p;
}

static hogp_map_result_t map_of(const uint8_t *d, size_t n, hogp_mouse_map_t *m) {
    uint8_t *c = exact_copy(d, n);
    hogp_map_result_t r = hogp_mouse_map_from_descriptor(c, n, m);
    free(c);
    return r;
}

static bool report_of(const hogp_mouse_map_t *m, const uint8_t *d, size_t n, uint8_t *b, int16_t *x, int16_t *y,
                      int8_t *w) {
    uint8_t *c = exact_copy(d, n);
    bool ok = hogp_parse_mouse_report(m, c, n, b, x, y, w);
    free(c);
    return ok;
}

static void test_boot_keyboard(void) {
    uint8_t in[8] = {1, 0, 4, 5, 0, 0, 0, 0}, out[8] = {0};
    uint8_t *c = exact_copy(in, 8);
    CHECK(hogp_parse_boot_keyboard(c, 8, out));
    CHECK(memcmp(in, out, 8) == 0);
    free(c);
    c = exact_copy(in, 7);
    CHECK(!hogp_parse_boot_keyboard(c, 7, out));
    free(c);
    uint8_t nine[9] = {0};
    CHECK(!hogp_parse_boot_keyboard(nine, 9, out));
    CHECK(!hogp_parse_boot_keyboard(NULL, 8, out));
    CHECK(!hogp_parse_boot_keyboard(in, 0, out));
}

static void test_boot_mouse(void) {
    uint8_t b = 0;
    int16_t dx = 0, dy = 0;
    const uint8_t r[3] = {0x05, 0xFF, 0x80};
    uint8_t *c = exact_copy(r, 3);
    CHECK(hogp_parse_boot_mouse(c, 3, &b, &dx, &dy));
    CHECK(b == 5 && dx == -1 && dy == -128);
    free(c);
    c = exact_copy(r, 2);
    CHECK(!hogp_parse_boot_mouse(c, 2, &b, &dx, &dy));
    free(c);
    const uint8_t longer[5] = {1, 2, 3, 4, 5};
    CHECK(hogp_parse_boot_mouse(longer, 5, &b, &dx, &dy) && dx == 2 && dy == 3);
}

static void test_wheel_mouse_map(void) {
    hogp_mouse_map_t m;
    memset(&m, 0xAA, sizeof m);
    CHECK(map_of(k_wheel_mouse, sizeof k_wheel_mouse, &m) == HOGP_MAP_OK);
    CHECK(!m.has_id && m.report_id == 0);
    CHECK(m.button_bits == 3 && m.x_bits == 8 && m.y_bits == 8 && m.wheel_bits == 8 && m.has_wheel);
    CHECK(m.x_off == 8 && m.y_off == 16 && m.wheel_off == 24);
    uint8_t b = 0;
    int16_t dx = 0, dy = 0;
    int8_t w = 0;
    const uint8_t rep[4] = {0x02, 0x10, 0xF0, 0xFF};
    CHECK(report_of(&m, rep, 4, &b, &dx, &dy, &w));
    CHECK(b == 2 && dx == 16 && dy == -16 && w == -1);
    CHECK(!report_of(&m, rep, 3, &b, &dx, &dy, &w));
    CHECK(report_of(&m, rep, 4, &b, &dx, &dy, NULL));
}

static void test_boot_mouse_map(void) {
    hogp_mouse_map_t m;
    CHECK(map_of(k_boot_mouse, sizeof k_boot_mouse, &m) == HOGP_MAP_OK);
    CHECK(!m.has_wheel && m.x_off == 8 && m.y_off == 16 && m.button_bits == 3);
    uint8_t b;
    int16_t dx, dy;
    int8_t w = 55;
    const uint8_t rep[3] = {1, 3, 0xFD};
    CHECK(report_of(&m, rep, 3, &b, &dx, &dy, &w));
    CHECK(b == 1 && dx == 3 && dy == -3 && w == 0);
}

static void test_logitech_12bit(void) {
    hogp_mouse_map_t m;
    CHECK(map_of(k_logitech, sizeof k_logitech, &m) == HOGP_MAP_OK);
    CHECK(m.has_id && m.report_id == 2);
    CHECK(m.button_bits == 16 && m.x_bits == 12 && m.y_bits == 12);
    CHECK(m.x_off == 16 && m.y_off == 28 && m.has_wheel && m.wheel_off == 40 && m.wheel_bits == 8);
    uint8_t b = 0;
    int16_t dx = 0, dy = 0;
    int8_t w = 0;
    /* buttons 0x0005, X = -1 (0xFFF), Y = +2047 (0x7FF), wheel -1, pan 0 */
    const uint8_t a[8] = {0x02, 0x05, 0x00, 0xFF, 0xFF, 0x7F, 0xFF, 0x00};
    CHECK(report_of(&m, a + 1, 7, &b, &dx, &dy, &w));
    CHECK(b == 5 && dx == -1 && dy == 2047 && w == -1);
    /* X = -2048 (0x800), Y = -1 (0xFFF), wheel +1 */
    const uint8_t c2[8] = {0x02, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x01, 0x00};
    CHECK(report_of(&m, c2 + 1, 7, &b, &dx, &dy, &w));
    CHECK(dx == -2048 && dy == -1 && w == 1);
    /* X = +1 (0x001), Y = -2 (0xFFE) */
    const uint8_t d[8] = {0x02, 0x00, 0x00, 0x01, 0xE0, 0xFF, 0x00, 0x00};
    CHECK(report_of(&m, d + 1, 7, &b, &dx, &dy, &w));
    CHECK(dx == 1 && dy == -2);
    /* wrong id and short reports are rejected, never mis-scaled */
    /* Payload is the bare notification (no ID byte; the ID comes from the Report Reference).
     * Short payloads are rejected, never mis-scaled. A payload that still carries an ID byte is
     * indistinguishable by content, so the contract is documented in the header. */
    CHECK(!report_of(&m, a + 1, 5, &b, &dx, &dy, &w));
    CHECK(report_of(&m, a + 1, 6, &b, &dx, &dy, &w));
    CHECK(report_of(&m, a + 1, 7, &b, &dx, &dy, &w)); /* trailing AC Pan byte is optional */
    CHECK(!report_of(&m, a + 1, 0, &b, &dx, &dy, &w));
    /* with a leading ID byte the fields shift: the ID byte must not be what the parser reads as buttons */
    CHECK(report_of(&m, a, 8, &b, &dx, &dy, &w) && b == 2); /* mis-parse is visible, hence the contract */
}

static void test_combo_and_rejections(void) {
    hogp_mouse_map_t m;
    CHECK(map_of(k_combo, sizeof k_combo, &m) == HOGP_MAP_OK);
    CHECK(m.has_id && m.report_id == 2 && m.x_off == 8 && m.y_off == 16 && m.wheel_off == 24);
    CHECK(map_of(k_keyboard, sizeof k_keyboard, &m) == HOGP_MAP_UNSUPPORTED);
    CHECK(map_of(k_absolute, sizeof k_absolute, &m) == HOGP_MAP_UNSUPPORTED);
    CHECK(map_of(k_wheel_mouse, sizeof k_wheel_mouse - 1, &m) == HOGP_MAP_MALFORMED);
    CHECK(map_of(k_wheel_mouse, 0, &m) == HOGP_MAP_MALFORMED);
    CHECK(hogp_mouse_map_from_descriptor(NULL, 4, &m) == HOGP_MAP_MALFORMED);
    /* long item (0xFE) is not interpreted */
    const uint8_t longitem[] = {0x05, 0x01, 0xFE, 0x02, 0x10, 0xAA, 0xBB};
    CHECK(map_of(longitem, sizeof longitem, &m) == HOGP_MAP_UNSUPPORTED);
    /* unsigned relative axes (logical min 0) cannot be scaled safely */
    uint8_t unsigned_axes[sizeof k_boot_mouse];
    memcpy(unsigned_axes, k_boot_mouse, sizeof unsigned_axes);
    CHECK(unsigned_axes[38] == 0x15 && unsigned_axes[39] == 0x81);
    unsigned_axes[39] = 0x00; /* logical minimum 0x81 -> 0 */
    CHECK(map_of(unsigned_axes, sizeof unsigned_axes, &m) == HOGP_MAP_UNSUPPORTED);
    /* axis wider than 16 bits */
    uint8_t wide[sizeof k_boot_mouse];
    memcpy(wide, k_boot_mouse, sizeof wide);
    CHECK(wide[42] == 0x75 && wide[43] == 0x08);
    wide[43] = 0x20; /* report size 32 */
    CHECK(map_of(wide, sizeof wide, &m) != HOGP_MAP_OK);
    /* pop with empty stack, and 17 pushes */
    const uint8_t pop[] = {0x05, 0x01, 0xB4};
    CHECK(map_of(pop, sizeof pop, &m) == HOGP_MAP_MALFORMED);
    uint8_t pushes[17];
    for (size_t i = 0; i < sizeof pushes; i++) { pushes[i] = 0xA4; }
    CHECK(map_of(pushes, sizeof pushes, &m) == HOGP_MAP_MALFORMED);
    /* unbalanced end collection */
    const uint8_t endc[] = {0xC0};
    CHECK(map_of(endc, 1, &m) == HOGP_MAP_MALFORMED);
    /* report ID 0 is invalid */
    const uint8_t id0[] = {0x85, 0x00};
    CHECK(map_of(id0, sizeof id0, &m) == HOGP_MAP_MALFORMED);
}

static void test_descriptor_longer_than_limit(void) {
    uint8_t big[600];
    memset(big, 0x05, sizeof big);
    hogp_mouse_map_t m;
    hogp_map_result_t r = map_of(big, sizeof big, &m);
    CHECK(r == HOGP_MAP_UNSUPPORTED || r == HOGP_MAP_MALFORMED);
    /* a mouse followed by junk past 512 bytes still parses: the walker stops at the mouse end */
    uint8_t padded[600];
    memset(padded, 0, sizeof padded);
    memcpy(padded, k_wheel_mouse, sizeof k_wheel_mouse);
    CHECK(map_of(padded, sizeof padded, &m) == HOGP_MAP_OK);
}

static void test_every_truncation(void) {
    const struct {
        const uint8_t *d;
        size_t n;
        bool mouse;
    } cases[] = {{k_wheel_mouse, sizeof k_wheel_mouse, true}, {k_boot_mouse, sizeof k_boot_mouse, true},
                 {k_logitech, sizeof k_logitech, true},       {k_keyboard, sizeof k_keyboard, false},
                 {k_absolute, sizeof k_absolute, false},      {k_combo, sizeof k_combo, true}};
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        for (size_t n = 0; n <= cases[c].n; n++) {
            hogp_mouse_map_t m;
            hogp_map_result_t r = map_of(cases[c].d, n, &m);
            CHECK(r == HOGP_MAP_OK || r == HOGP_MAP_UNSUPPORTED || r == HOGP_MAP_MALFORMED);
            if (n < cases[c].n) { CHECK(r != HOGP_MAP_OK); }
            if (n == cases[c].n && cases[c].mouse) { CHECK(r == HOGP_MAP_OK); }
        }
    }
}

static void test_random_bytes_never_overread(void) {
    uint32_t s = 12345u;
    for (int iter = 0; iter < 20000; iter++) {
        s = s * 1664525u + 1013904223u;
        size_t n = (s >> 16) % 80;
        uint8_t *p = (uint8_t *)malloc(n ? n : 1);
        if (p == NULL) { exit(2); }
        for (size_t i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            p[i] = (uint8_t)(s >> 24);
        }
        hogp_mouse_map_t m;
        hogp_map_result_t r = hogp_mouse_map_from_descriptor(p, n, &m);
        CHECK(r == HOGP_MAP_OK || r == HOGP_MAP_UNSUPPORTED || r == HOGP_MAP_MALFORMED);
        free(p);
    }
    /* mutate real descriptors: flip one byte at every position to every sampled value */
    const uint8_t vals[] = {0x00, 0x01, 0x07, 0x08, 0x10, 0x20, 0x7F, 0x80, 0xA1, 0xA4, 0xB4, 0xC0, 0xFE, 0xFF};
    const uint8_t *srcs[] = {k_logitech, k_combo, k_wheel_mouse};
    const size_t lens[] = {sizeof k_logitech, sizeof k_combo, sizeof k_wheel_mouse};
    for (size_t k = 0; k < 3; k++) {
        for (size_t i = 0; i < lens[k]; i++) {
            for (size_t v = 0; v < sizeof vals; v++) {
                uint8_t *p = exact_copy(srcs[k], lens[k]);
                p[i] = vals[v];
                hogp_mouse_map_t m;
                hogp_map_result_t r = hogp_mouse_map_from_descriptor(p, lens[k], &m);
                CHECK(r == HOGP_MAP_OK || r == HOGP_MAP_UNSUPPORTED || r == HOGP_MAP_MALFORMED);
                if (r == HOGP_MAP_OK) {
                    uint8_t rep[24] = {0};
                    uint8_t b;
                    int16_t x, y;
                    int8_t w;
                    uint8_t *rc = exact_copy(rep, sizeof rep);
                    rep[0] = 0;
                    (void)hogp_parse_mouse_report(&m, rc, sizeof rep, &b, &x, &y, &w);
                    free(rc);
                }
                free(p);
            }
        }
    }
}

int main(void) {
    test_boot_keyboard();
    test_boot_mouse();
    test_wheel_mouse_map();
    test_boot_mouse_map();
    test_logitech_12bit();
    test_combo_and_rejections();
    test_descriptor_longer_than_limit();
    test_every_truncation();
    test_random_bytes_never_overread();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
