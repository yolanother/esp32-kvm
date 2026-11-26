/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the pure USB boot-mode decision and the magic-string matcher used to
 * request a serial reboot. No platform state or ESP-IDF headers live here. */
#ifndef ESP32_KVM_USB_MODE_H
#define ESP32_KVM_USB_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { KVM_USB_MODE_SERIAL_JTAG = 0, KVM_USB_MODE_COMPOSITE = 1 } kvm_usb_mode_t;

typedef enum {
    KVM_USB_REASON_DEFAULT,
    KVM_USB_REASON_STORED,
    KVM_USB_REASON_NOT_BUILT,
    KVM_USB_REASON_FORCED_BY_BUILD,
    KVM_USB_REASON_SERIAL_ONCE,
    KVM_USB_REASON_ESCAPE_BUTTON,
    KVM_USB_REASON_ABNORMAL_RESET,
    KVM_USB_REASON_UNCONFIRMED_LIMIT
} kvm_usb_reason_t;

/** Facts gathered at boot; the decision reads nothing else. */
typedef struct {
    bool composite_built;
    bool force_serial;
    bool serial_once;
    bool escape_held;
    bool abnormal_reset;
    uint8_t stored_mode;
    uint8_t unconfirmed;
    uint8_t max_unconfirmed;
} kvm_usb_boot_inputs_t;

typedef struct {
    kvm_usb_mode_t mode;
    kvm_usb_reason_t reason;
    bool persist_serial;  /* write mode = 0 to storage */
    bool count_attempt;   /* increment unconfirmed before starting composite */
} kvm_usb_boot_decision_t;

/** Applies the spec priority table; NULL input yields serial with DEFAULT. */
kvm_usb_boot_decision_t kvm_usb_mode_decide(const kvm_usb_boot_inputs_t *in);

/** True only when, ignoring escape_held, the decision would be composite.
 *  Derived from kvm_usb_mode_decide, so the two can never disagree. */
bool kvm_usb_mode_needs_window(const kvm_usb_boot_inputs_t *in);

/** Escape-window sampler (spec Decision 8): BOOT counts once it has been seen
 *  down continuously for hold_ms; only samples taken before window_ms count. */
typedef struct {
    uint32_t window_ms;
    uint32_t hold_ms;
    bool down;            /* last sample was down */
    uint32_t down_since;  /* elapsed time of the first down sample of this press */
    bool held;            /* result: a qualifying hold was seen */
} kvm_usb_escape_t;

void kvm_usb_escape_init(kvm_usb_escape_t *e, uint32_t window_ms, uint32_t hold_ms);

/** Feeds one BOOT sample taken elapsed_ms after the window opened (monotonic).
 *  Returns true while the caller should keep sampling: false once a hold has
 *  been counted (e->held) or when elapsed_ms >= window_ms (that sample is
 *  ignored). A release restarts the hold. NULL returns false. */
bool kvm_usb_escape_sample(kvm_usb_escape_t *e, bool boot_down, uint32_t elapsed_ms);

/** Raw host-to-device magic strings (spec Decision 9), 21 bytes each without
 *  the literal's terminating NUL: restart once in serial, persist serial.
 *  Why protocol traffic cannot trigger them: the binary protocol is COBS
 *  framed, so a 0 byte is only ever a frame delimiter and the byte after it is
 *  the next frame's first COBS code byte. Every valid frame starts with the
 *  header 56 4B 01 kind len_lo len_hi 00 00 (flags:u16 = 0, which the core
 *  requires, protocol/schema/v1.md), so that code
 *  byte is at most 7, never 'K' (0x4B), and the second magic byte can never
 *  match after a real delimiter. Only a deliberate write (or 21 exact garbage
 *  bytes, about 2^-168 per position) matches. */
#define KVM_USB_MAGIC_SERIAL_ONCE "\0KVM-USB-SERIAL-ONCE\n"
#define KVM_USB_MAGIC_SERIAL_MODE "\0KVM-USB-SERIAL-MODE\n"

/** Incremental exact matcher for a byte pattern that may contain 0 bytes. */
typedef struct {
    const char *pattern;
    size_t length;
    size_t matched;
} kvm_usb_magic_t;

void kvm_usb_magic_init(kvm_usb_magic_t *m, const char *pattern, size_t length);

/** Feeds bytes; returns true on the first full match in this call and then
 *  restarts from an empty state (bytes after the match in the same call are
 *  not examined). Correct for any pattern, self-overlapping or not: on a
 *  mismatch it falls back to the longest pattern prefix that ends the bytes
 *  seen so far (KMP fallback computed from the pattern, no table). For the
 *  zero-led magic strings, whose only 0 is the first byte, this is exactly the
 *  diagnostics matcher's rule (a 0 restarts the match at 1). */
bool kvm_usb_magic_feed(kvm_usb_magic_t *m, const uint8_t *bytes, size_t n);

#endif
