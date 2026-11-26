/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises the pure USB boot decision in spec priority order and the magic
 * string matcher, including split feeds and mid-pattern zero bytes. */
#include "usb_mode.h"
#include <assert.h>
#include <string.h>

static kvm_usb_boot_inputs_t base(void)
{
    kvm_usb_boot_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.composite_built = true;
    in.stored_mode = 1;
    in.max_unconfirmed = 3;
    return in;
}

static void test_priority(void)
{
    kvm_usb_boot_inputs_t in = base();
    kvm_usb_boot_decision_t d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_COMPOSITE && d.reason == KVM_USB_REASON_STORED);
    assert(d.count_attempt && !d.persist_serial);

    in = base(); in.composite_built = false; in.force_serial = true; in.serial_once = true;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_NOT_BUILT);
    assert(!d.count_attempt && !d.persist_serial);

    in = base(); in.force_serial = true; in.serial_once = true; in.escape_held = true;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_FORCED_BY_BUILD);
    assert(!d.count_attempt && !d.persist_serial);

    in = base(); in.serial_once = true; in.escape_held = true; in.abnormal_reset = true; in.unconfirmed = 3;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_SERIAL_ONCE);
    assert(!d.count_attempt && !d.persist_serial);

    in = base(); in.escape_held = true; in.abnormal_reset = true; in.unconfirmed = 3;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_ESCAPE_BUTTON);
    assert(!d.persist_serial && !d.count_attempt);

    in = base(); in.abnormal_reset = true; in.unconfirmed = 3;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_ABNORMAL_RESET);
    assert(!d.count_attempt && !d.persist_serial);

    in = base(); in.unconfirmed = 3;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_UNCONFIRMED_LIMIT);
    assert(d.persist_serial && !d.count_attempt);

    in = base(); in.unconfirmed = 2;
    d = kvm_usb_mode_decide(&in);
    assert(d.mode == KVM_USB_MODE_COMPOSITE && d.reason == KVM_USB_REASON_STORED && d.count_attempt);

    const uint8_t others[] = { 0, 2, 255 };
    for (size_t i = 0; i < sizeof(others); ++i) {
        in = base(); in.stored_mode = others[i];
        d = kvm_usb_mode_decide(&in);
        assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_DEFAULT);
        assert(!d.count_attempt && !d.persist_serial);
    }

    d = kvm_usb_mode_decide(NULL);
    assert(d.mode == KVM_USB_MODE_SERIAL_JTAG && d.reason == KVM_USB_REASON_DEFAULT);
}

static void test_needs_window(void)
{
    kvm_usb_boot_inputs_t in = base();
    assert(kvm_usb_mode_needs_window(&in));
    in.escape_held = true;
    assert(kvm_usb_mode_needs_window(&in));
    in = base(); in.composite_built = false; assert(!kvm_usb_mode_needs_window(&in));
    in = base(); in.force_serial = true; assert(!kvm_usb_mode_needs_window(&in));
    in = base(); in.serial_once = true; assert(!kvm_usb_mode_needs_window(&in));
    in = base(); in.abnormal_reset = true; assert(!kvm_usb_mode_needs_window(&in));
    in = base(); in.unconfirmed = 3; assert(!kvm_usb_mode_needs_window(&in));
    in = base(); in.stored_mode = 0; assert(!kvm_usb_mode_needs_window(&in));
    assert(!kvm_usb_mode_needs_window(NULL));
}

static const char ONCE[] = "\0KVM-USB-SERIAL-ONCE\n";
static const char MODE[] = "\0KVM-USB-SERIAL-MODE\n";
#define ONCE_LEN (sizeof(ONCE) - 1)
#define MODE_LEN (sizeof(MODE) - 1)

static void test_magic(void)
{
    kvm_usb_magic_t m;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    assert(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE, 7));
    assert(kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + 7, ONCE_LEN - 7));
    assert(!kvm_usb_magic_feed(&m, (const uint8_t *)"x", 1));

    /* Embedded in frame bytes. */
    uint8_t frame[64];
    size_t n = 0;
    memset(frame, 0xA5, 10); n = 10;
    memcpy(frame + n, ONCE, ONCE_LEN); n += ONCE_LEN;
    memset(frame + n, 0x5A, 5); n += 5;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    assert(kvm_usb_magic_feed(&m, frame, n));

    /* A zero byte mid-pattern restarts at 1 and the match still completes. */
    uint8_t restart[64];
    memcpy(restart, ONCE, 8); n = 8;
    memcpy(restart + n, ONCE, ONCE_LEN); n += ONCE_LEN;
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    assert(kvm_usb_magic_feed(&m, restart, n));

    /* Partial then unrelated byte does not match. */
    kvm_usb_magic_init(&m, ONCE, ONCE_LEN);
    assert(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE, 10));
    assert(!kvm_usb_magic_feed(&m, (const uint8_t *)"Z", 1));
    assert(!kvm_usb_magic_feed(&m, (const uint8_t *)ONCE + 11, ONCE_LEN - 11));

    /* Two patterns fed the same bytes fire independently. */
    kvm_usb_magic_t once, mode;
    kvm_usb_magic_init(&once, ONCE, ONCE_LEN);
    kvm_usb_magic_init(&mode, MODE, MODE_LEN);
    assert(kvm_usb_magic_feed(&once, (const uint8_t *)ONCE, ONCE_LEN));
    assert(!kvm_usb_magic_feed(&mode, (const uint8_t *)ONCE, ONCE_LEN));
    assert(kvm_usb_magic_feed(&mode, (const uint8_t *)MODE, MODE_LEN));
    assert(!kvm_usb_magic_feed(&once, (const uint8_t *)MODE, MODE_LEN));

    /* Empty feed is harmless. */
    assert(!kvm_usb_magic_feed(&once, NULL, 0));
}

int main(void)
{
    test_priority();
    test_needs_window();
    test_magic();
    return 0;
}
