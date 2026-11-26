/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the pure USB boot decision in spec priority order (every row checks
 * all four decision fields), the escape-window sampler, a multi-boot trace of
 * the unconfirmed counter, and the magic string matcher (split feeds, mid-pattern
 * zero bytes, self-overlapping patterns, independence from the diagnostics
 * pattern). CHECK stays active under NDEBUG. */
#include "usb_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                     \
        }                                                                \
    } while (0)

static kvm_usb_boot_inputs_t base(void)
{
    kvm_usb_boot_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.composite_built = true;
    in.stored_mode = 1;
    in.max_unconfirmed = 3;
    return in;
}

static void expect(kvm_usb_boot_decision_t d, kvm_usb_mode_t mode, kvm_usb_reason_t reason,
                   bool persist_serial, bool count_attempt, int line)
{
    if (d.mode != mode || d.reason != reason || d.persist_serial != persist_serial ||
        d.count_attempt != count_attempt) {
        printf("FAIL %s:%d decision {mode %d reason %d persist %d count %d}, expected "
               "{%d %d %d %d}\n", __FILE__, line, (int)d.mode, (int)d.reason,
               (int)d.persist_serial, (int)d.count_attempt, (int)mode, (int)reason,
               (int)persist_serial, (int)count_attempt);
        exit(1);
    }
}
#define EXPECT(in, mode, reason, persist, count) \
    expect(kvm_usb_mode_decide(&(in)), mode, reason, persist, count, __LINE__)
#define SERIAL KVM_USB_MODE_SERIAL_JTAG
#define COMPOSITE KVM_USB_MODE_COMPOSITE

static void test_priority(void)
{
    kvm_usb_boot_inputs_t in = base();
    EXPECT(in, COMPOSITE, KVM_USB_REASON_STORED, false, true);

    in = base(); in.composite_built = false; in.force_serial = true; in.serial_once = true;
    EXPECT(in, SERIAL, KVM_USB_REASON_NOT_BUILT, false, false);
    in = base(); in.composite_built = false; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_NOT_BUILT, false, false);

    in = base(); in.force_serial = true; in.serial_once = true; in.escape_held = true;
    EXPECT(in, SERIAL, KVM_USB_REASON_FORCED_BY_BUILD, false, false);
    in = base(); in.force_serial = true; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_FORCED_BY_BUILD, false, false);

    in = base(); in.serial_once = true; in.escape_held = true; in.abnormal_reset = true; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_SERIAL_ONCE, false, false);

    in = base(); in.escape_held = true; in.abnormal_reset = true; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_ESCAPE_BUTTON, false, false);

    in = base(); in.abnormal_reset = true; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_ABNORMAL_RESET, false, false);

    in = base(); in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);
    in = base(); in.unconfirmed = 4;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);
    in = base(); in.unconfirmed = 255;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);
    /* The limit row precedes the stored-mode rows: it also fires with mode 0. */
    in = base(); in.stored_mode = 0; in.unconfirmed = 3;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);

    in = base(); in.unconfirmed = 2;
    EXPECT(in, COMPOSITE, KVM_USB_REASON_STORED, false, true);

    /* max_unconfirmed bounds (Kconfig keeps it in 1..255). */
    in = base(); in.max_unconfirmed = 0;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);
    in = base(); in.max_unconfirmed = 1;
    EXPECT(in, COMPOSITE, KVM_USB_REASON_STORED, false, true);
    in = base(); in.max_unconfirmed = 1; in.unconfirmed = 1;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);
    in = base(); in.max_unconfirmed = 255; in.unconfirmed = 254;
    EXPECT(in, COMPOSITE, KVM_USB_REASON_STORED, false, true);
    in = base(); in.max_unconfirmed = 255; in.unconfirmed = 255;
    EXPECT(in, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false);

    const uint8_t others[] = { 0, 2, 255 };
    for (size_t i = 0; i < sizeof(others); ++i) {
        in = base(); in.stored_mode = others[i];
        EXPECT(in, SERIAL, KVM_USB_REASON_DEFAULT, false, false);
    }

    expect(kvm_usb_mode_decide(NULL), SERIAL, KVM_USB_REASON_DEFAULT, false, false, __LINE__);
}

static void test_needs_window(void)
{
    kvm_usb_boot_inputs_t in = base();
    CHECK(kvm_usb_mode_needs_window(&in));
    in.escape_held = true;
    CHECK(kvm_usb_mode_needs_window(&in));
    in = base(); in.unconfirmed = 2; CHECK(kvm_usb_mode_needs_window(&in));
    in = base(); in.composite_built = false; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.force_serial = true; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.serial_once = true; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.abnormal_reset = true; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.unconfirmed = 3; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.unconfirmed = 255; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.max_unconfirmed = 0; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.stored_mode = 0; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.stored_mode = 2; CHECK(!kvm_usb_mode_needs_window(&in));
    in = base(); in.stored_mode = 255; CHECK(!kvm_usb_mode_needs_window(&in));
    CHECK(!kvm_usb_mode_needs_window(NULL));

    /* needs_window agrees with the decision for every input combination. */
    for (unsigned bits = 0; bits < 32; ++bits) {
        const uint8_t modes[] = { 0, 1, 2 };
        const uint8_t counts[] = { 0, 2, 3, 255 };
        for (size_t m = 0; m < sizeof(modes); ++m) {
            for (size_t c = 0; c < sizeof(counts); ++c) {
                in = base();
                in.composite_built = (bits & 1u) != 0;
                in.force_serial = (bits & 2u) != 0;
                in.serial_once = (bits & 4u) != 0;
                in.escape_held = (bits & 8u) != 0;
                in.abnormal_reset = (bits & 16u) != 0;
                in.stored_mode = modes[m];
                in.unconfirmed = counts[c];
                kvm_usb_boot_inputs_t no_escape = in;
                no_escape.escape_held = false;
                bool composite = kvm_usb_mode_decide(&no_escape).mode == COMPOSITE;
                CHECK(kvm_usb_mode_needs_window(&in) == composite);
                /* The escape button can only turn a composite boot into serial. */
                if (kvm_usb_mode_decide(&in).mode == COMPOSITE) CHECK(!in.escape_held && composite);
            }
        }
    }
}

/* Simulated NVS: what kvm_usb_boot_decide applies after each decision. */
typedef struct { uint8_t mode; uint8_t unconf; } fake_store_t;

static kvm_usb_boot_decision_t boot(fake_store_t *store, bool escape, bool abnormal)
{
    kvm_usb_boot_inputs_t in = base();
    in.stored_mode = store->mode;
    in.unconfirmed = store->unconf;
    in.escape_held = escape;
    in.abnormal_reset = abnormal;
    kvm_usb_boot_decision_t d = kvm_usb_mode_decide(&in);
    if (d.persist_serial) store->mode = 0;
    if (d.count_attempt) store->unconf = (uint8_t)(store->unconf + 1);
    return d;
}

static void test_three_short_boots_trace(void)
{
    /* Composite enabled from the screen: mode 1, counter cleared. */
    fake_store_t store = { 1, 0 };
    for (uint8_t i = 1; i <= 3; ++i) {
        kvm_usb_boot_decision_t d = boot(&store, false, false);
        expect(d, COMPOSITE, KVM_USB_REASON_STORED, false, true, __LINE__);
        CHECK(store.unconf == i && store.mode == 1); /* written before TinyUSB starts */
        /* Unplugged within 30 s: no confirmation, counter keeps its value. */
    }
    kvm_usb_boot_decision_t d = boot(&store, false, false);
    expect(d, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false, __LINE__);
    CHECK(store.mode == 0 && store.unconf == 3);
    /* Every later boot stays serial; persisting mode 0 again changes nothing. */
    d = boot(&store, false, false);
    expect(d, SERIAL, KVM_USB_REASON_UNCONFIRMED_LIMIT, true, false, __LINE__);
    CHECK(store.mode == 0 && store.unconf == 3);

    /* Escape and abnormal resets do not count an attempt. */
    store = (fake_store_t){ 1, 0 };
    d = boot(&store, true, false);
    expect(d, SERIAL, KVM_USB_REASON_ESCAPE_BUTTON, false, false, __LINE__);
    d = boot(&store, false, true);
    expect(d, SERIAL, KVM_USB_REASON_ABNORMAL_RESET, false, false, __LINE__);
    CHECK(store.mode == 1 && store.unconf == 0);

    /* A confirmed boot (30 s mounted) clears the counter and restarts the budget. */
    store = (fake_store_t){ 1, 2 };
    d = boot(&store, false, false);
    expect(d, COMPOSITE, KVM_USB_REASON_STORED, false, true, __LINE__);
    CHECK(store.unconf == 3);
    store.unconf = 0; /* confirmation */
    for (int i = 0; i < 3; ++i) CHECK(boot(&store, false, false).mode == COMPOSITE);
    CHECK(boot(&store, false, false).mode == SERIAL && store.mode == 0);
}

/* Runs the sampler the way usb_boot.c does: one sample every 10 ms. Returns
 * the number of samples taken. down(t) gives the BOOT level at elapsed t. */
static unsigned run_window(kvm_usb_escape_t *e, bool (*down)(uint32_t), uint32_t step)
{
    unsigned samples = 0;
    for (uint32_t t = 0;; t += step) {
        bool more = kvm_usb_escape_sample(e, down(t), t);
        if (!more) break;
        ++samples;
        CHECK(samples < 100000);
    }
    return samples;
}

static bool never(uint32_t t) { (void)t; return false; }
static bool always(uint32_t t) { (void)t; return true; }
static bool from_500(uint32_t t) { return t >= 500; }
static bool short_190(uint32_t t) { return t >= 100 && t < 290; }
static bool exact_200(uint32_t t) { return t >= 100 && t <= 300; }
static bool bouncing(uint32_t t) { return (t / 10) % 15 != 14; } /* 140 ms down, 10 ms up */
static bool late_1350(uint32_t t) { return t >= 1350; }
static bool late_1290(uint32_t t) { return t >= 1290; }

static void test_escape_window(void)
{
    kvm_usb_escape_t e;
    kvm_usb_escape_init(&e, 1500, 200);
    CHECK(run_window(&e, never, 10) == 150 && !e.held); /* samples at 0..1490 */

    kvm_usb_escape_init(&e, 1500, 200);
    CHECK(run_window(&e, always, 10) == 20 && e.held); /* counted at t = 200 */

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, from_500, 10);
    CHECK(e.held);

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, short_190, 10);
    CHECK(!e.held); /* 190 ms is not a hold */

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, exact_200, 10);
    CHECK(e.held); /* down at 100, still down at 300: 200 ms */

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, bouncing, 10);
    CHECK(!e.held); /* a release restarts the hold */

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, late_1290, 10);
    CHECK(e.held); /* 1290 + 200 = 1490, the last sample inside the window */

    kvm_usb_escape_init(&e, 1500, 200);
    run_window(&e, late_1350, 10);
    CHECK(!e.held); /* would need t = 1550: the window has closed */

    /* A sample at or past the window end is ignored and stops sampling. */
    kvm_usb_escape_init(&e, 1500, 0);
    CHECK(!kvm_usb_escape_sample(&e, true, 1500) && !e.held);

    /* Zero hold counts the first down sample; zero window samples nothing. */
    kvm_usb_escape_init(&e, 1500, 0);
    CHECK(!kvm_usb_escape_sample(&e, true, 0) && e.held);
    kvm_usb_escape_init(&e, 0, 200);
    CHECK(run_window(&e, always, 10) == 0 && !e.held);

    /* Once held, further samples change nothing. */
    kvm_usb_escape_init(&e, 1500, 0);
    CHECK(!kvm_usb_escape_sample(&e, true, 0));
    CHECK(!kvm_usb_escape_sample(&e, false, 10) && e.held);

    /* Irregular sampling (a late wake-up) still measures real elapsed time. */
    kvm_usb_escape_init(&e, 1500, 200);
    CHECK(kvm_usb_escape_sample(&e, true, 0));
    CHECK(!kvm_usb_escape_sample(&e, true, 250) && e.held);

    /* NULL is harmless. */
    kvm_usb_escape_init(NULL, 1500, 200);
    CHECK(!kvm_usb_escape_sample(NULL, true, 0));
}

static const char ONCE[] = "\0KVM-USB-SERIAL-ONCE\n";
static const char MODE[] = "\0KVM-USB-SERIAL-MODE\n";
static const char DIAG[] = "\0KVM-DIAG-LOGS\n";
#define ONCE_LEN (sizeof(ONCE) - 1)
#define MODE_LEN (sizeof(MODE) - 1)
#define DIAG_LEN (sizeof(DIAG) - 1)

static void test_magic(void)
{
    /* 21 pattern bytes (sizeof is 22 with the literal's terminating NUL). */
    const size_t once_len = ONCE_LEN, mode_len = MODE_LEN;
    CHECK(once_len == 21 && mode_len == 21);
    /* The firmware's macros are the plan's strings verbatim. */
    CHECK(sizeof(KVM_USB_MAGIC_SERIAL_ONCE) == sizeof(ONCE) &&
          memcmp(KVM_USB_MAGIC_SERIAL_ONCE, ONCE, sizeof(ONCE)) == 0);
    CHECK(sizeof(KVM_USB_MAGIC_SERIAL_MODE) == sizeof(MODE) &&
          memcmp(KVM_USB_MAGIC_SERIAL_MODE, MODE, sizeof(MODE)) == 0);
    kvm_usb_magic_t m;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE, 7));
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + 7, ONCE_LEN - 7));
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)"x", 1));

    /* Byte at a time. */
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    for (size_t i = 0; i + 1 < ONCE_LEN; ++i) CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + i, 1));
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + ONCE_LEN - 1, 1));

    /* Embedded in frame bytes. */
    uint8_t frame[64];
    size_t n = 0;
    memset(frame, 0xA5, 10); n = 10;
    memcpy(frame + n, ONCE, ONCE_LEN); n += ONCE_LEN;
    memset(frame + n, 0x5A, 5); n += 5;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(kvm_usb_magic_feed(&m, frame, n));

    /* A zero byte mid-pattern restarts at 1 and the match still completes. */
    uint8_t restart[64];
    memcpy(restart, ONCE, 8); n = 8;
    memcpy(restart + n, ONCE, ONCE_LEN); n += ONCE_LEN;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(kvm_usb_magic_feed(&m, restart, n));

    /* Several zero bytes (COBS frame delimiters, empty frames) before the magic. */
    uint8_t zeros[64] = {0};
    memcpy(zeros + 5, ONCE, ONCE_LEN);
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(kvm_usb_magic_feed(&m, zeros, 5 + ONCE_LEN));

    /* Partial then unrelated byte does not match. */
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE, 10));
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)"Z", 1));
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + 11, ONCE_LEN - 11));

    /* Without the leading zero it never matches. */
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + 1, ONCE_LEN - 1));

    /* Two patterns fed the same bytes fire independently. */
    kvm_usb_magic_t once, mode;
    kvm_usb_magic_init(&once, ONCE, ONCE_LEN);
    kvm_usb_magic_init(&mode, MODE, MODE_LEN);
    CHECK(kvm_usb_magic_feed(&once, (const uint8_t *)ONCE, ONCE_LEN));
    CHECK(!kvm_usb_magic_feed(&mode, (const uint8_t *)ONCE, ONCE_LEN));
    CHECK(kvm_usb_magic_feed(&mode, (const uint8_t *)MODE, MODE_LEN));
    CHECK(!kvm_usb_magic_feed(&once, (const uint8_t *)MODE, MODE_LEN));

    /* The diagnostics command shares the "\0KVM-" prefix: the three matchers
     * fed the same stream each fire only on their own pattern. */
    kvm_usb_magic_t diag;
    kvm_usb_magic_init(&once, ONCE, ONCE_LEN);
    kvm_usb_magic_init(&mode, MODE, MODE_LEN);
    kvm_usb_magic_init(&diag, DIAG, DIAG_LEN);
    CHECK(kvm_usb_magic_feed(&diag, (const uint8_t *)DIAG, DIAG_LEN));
    CHECK(!kvm_usb_magic_feed(&once, (const uint8_t *)DIAG, DIAG_LEN));
    CHECK(!kvm_usb_magic_feed(&mode, (const uint8_t *)DIAG, DIAG_LEN));
    CHECK(!kvm_usb_magic_feed(&diag, (const uint8_t *)ONCE, ONCE_LEN));
    CHECK(!kvm_usb_magic_feed(&diag, (const uint8_t *)MODE, MODE_LEN));
    CHECK(kvm_usb_magic_feed(&diag, (const uint8_t *)DIAG, DIAG_LEN));
    CHECK(kvm_usb_magic_feed(&once, (const uint8_t *)ONCE, ONCE_LEN));
    CHECK(kvm_usb_magic_feed(&mode, (const uint8_t *)MODE, MODE_LEN));

    /* Empty feed and bad matchers are harmless. */
    CHECK(!kvm_usb_magic_feed(&once, NULL, 0));
    CHECK(!kvm_usb_magic_feed(NULL, (const uint8_t *)ONCE, ONCE_LEN));
    kvm_usb_magic_init(&m, NULL, 0);
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE, ONCE_LEN));
}

/* Valid protocol traffic is COBS: 0 appears only as a frame delimiter and the
 * byte after it is the next frame's first code byte, at most 8 for a header
 * that starts 56 4B 01 kind len16 00 00. The real vectors never trigger. */
static void test_magic_not_triggered_by_frames(void)
{
    static const uint8_t arm[] = {
        0x06, 0x56, 0x4b, 0x01, 0x22, 0x05, 0x01, 0x01, 0x02, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x02, 0x01, 0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0x03, 0x01, 0x02, 0x01, 0x01, 0x05, 0xa4, 0x6e,
        0x92, 0xb2, 0x00,
    };
    kvm_usb_magic_t once, mode;
    kvm_usb_magic_init(&once, ONCE, ONCE_LEN);
    kvm_usb_magic_init(&mode, MODE, MODE_LEN);
    for (int i = 0; i < 1000; ++i) {
        CHECK(!kvm_usb_magic_feed(&once, arm, sizeof(arm)));
        CHECK(!kvm_usb_magic_feed(&mode, arm, sizeof(arm)));
    }
    /* Pseudo-random bytes (xorshift) with frequent zeros: no false match. */
    uint32_t x = 0x12345678u;
    uint8_t chunk[128];
    for (int round = 0; round < 20000; ++round) {
        for (size_t i = 0; i < sizeof(chunk); ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            chunk[i] = (x & 7u) == 0 ? 0 : (uint8_t)(x >> 8);
        }
        CHECK(!kvm_usb_magic_feed(&once, chunk, sizeof(chunk)));
        CHECK(!kvm_usb_magic_feed(&mode, chunk, sizeof(chunk)));
    }
    /* After all that traffic the magic still matches at once. */
    uint8_t tail[1 + ONCE_LEN];
    tail[0] = 0x00;
    memcpy(tail + 1, ONCE, ONCE_LEN);
    CHECK(kvm_usb_magic_feed(&once, tail, sizeof(tail)));
}

/* The matcher is correct for any pattern, not only zero-led ones. */
static void test_magic_general_patterns(void)
{
    kvm_usb_magic_t m;
    kvm_usb_magic_init(&m, "aab", 3);
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"aaab", 4));
    kvm_usb_magic_init(&m, "abab", 4);
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"abaabab", 7));
    kvm_usb_magic_init(&m, "abac", 4);
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"ababac", 6));
    /* After a match the matcher restarts: overlapping occurrences need fresh bytes. */
    kvm_usb_magic_init(&m, "aa", 2);
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"aa", 2));
    CHECK(!kvm_usb_magic_feed(&m, (const uint8_t *)"a", 1));
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"a", 1));
    /* A pattern with interior zeros. */
    static const char z[] = "\0a\0ab";
    kvm_usb_magic_init(&m, z, 5);
    CHECK(kvm_usb_magic_feed(&m, (const uint8_t *)"\0a\0a\0ab", 7));

    /* Compare against a brute-force search on pseudo-random two-letter text. */
    static const char *patterns[] = { "abab", "aab", "abaab", "aaaa", "baab" };
    uint32_t x = 0xC0FFEEu;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); ++p) {
        size_t len = strlen(patterns[p]);
        for (int trial = 0; trial < 2000; ++trial) {
            char text[24];
            for (size_t i = 0; i < sizeof(text); ++i) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                text[i] = (x & 1u) ? 'a' : 'b';
            }
            /* Reference: first end index of an occurrence, matching restarts after it. */
            size_t expected = 0, start = 0;
            for (size_t end = len; end <= sizeof(text); ++end) {
                if (end - len >= start && memcmp(text + end - len, patterns[p], len) == 0) {
                    ++expected;
                    start = end;
                }
            }
            size_t got = 0;
            kvm_usb_magic_init(&m, patterns[p], len);
            for (size_t i = 0; i < sizeof(text); ++i)
                if (kvm_usb_magic_feed(&m, (const uint8_t *)text + i, 1)) ++got;
            CHECK(got == expected);
        }
    }
}

/* The boot snapshot line is the drill's evidence (task 9): it must carry the
 * inputs the decision used, notably once=1 on a NOT_BUILT image. */
static void test_boot_describe(void)
{
    kvm_usb_boot_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.inputs.serial_once = true;
    s.inputs.max_unconfirmed = 3;
    s.inputs.stored_mode = 0;
    s.decision = kvm_usb_mode_decide(&s.inputs); /* not built: NOT_BUILT */
    s.reset_reason = 3;
    char line[128];
    int n = kvm_usb_boot_describe(&s, line, sizeof(line));
    const char *want = "mode=0 reason=2 once=1 escape=0 abnormal=0 rst=3 stored=0 unconf=0";
    CHECK(strcmp(line, want) == 0);
    CHECK(n == (int)strlen(want));

    s.inputs.composite_built = true;
    s.inputs.serial_once = false;
    s.inputs.abnormal_reset = true;
    s.inputs.stored_mode = 1;
    s.inputs.unconfirmed = 2;
    s.decision = kvm_usb_mode_decide(&s.inputs);
    s.reset_reason = 4;
    n = kvm_usb_boot_describe(&s, line, sizeof(line));
    CHECK(strcmp(line, "mode=0 reason=6 once=0 escape=0 abnormal=1 rst=4 stored=1 unconf=2") == 0);

    s.inputs.abnormal_reset = false;
    s.inputs.escape_held = true;
    s.decision = kvm_usb_mode_decide(&s.inputs);
    s.reset_reason = 3;
    (void)kvm_usb_boot_describe(&s, line, sizeof(line));
    CHECK(strcmp(line, "mode=0 reason=5 once=0 escape=1 abnormal=0 rst=3 stored=1 unconf=2") == 0);

    /* Truncation always leaves a terminated string; bad arguments write nothing. */
    char small[8];
    memset(small, 'x', sizeof(small));
    n = kvm_usb_boot_describe(&s, small, sizeof(small));
    CHECK(n > (int)sizeof(small) && small[sizeof(small) - 1] == '\0' && strncmp(small, "mode=0 ", 7) == 0);
    CHECK(kvm_usb_boot_describe(NULL, line, sizeof(line)) == 0);
    CHECK(kvm_usb_boot_describe(&s, NULL, 16) == 0);
    line[0] = 'x';
    CHECK(kvm_usb_boot_describe(&s, line, 0) == 0 && line[0] == 'x');
}

int main(void)
{
    test_priority();
    test_needs_window();
    test_three_short_boots_trace();
    test_escape_window();
    test_magic();
    test_magic_not_triggered_by_frames();
    test_magic_general_patterns();
    test_boot_describe();
    printf("test_usb_mode: all checks passed\n");
    return 0;
}
