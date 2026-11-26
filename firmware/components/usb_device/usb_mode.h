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

/** True only when, ignoring escape_held, the decision would be composite. */
bool kvm_usb_mode_needs_window(const kvm_usb_boot_inputs_t *in);

/** Incremental matcher for a byte pattern that may contain a leading 0. */
typedef struct {
    const char *pattern;
    size_t length;
    size_t matched;
} kvm_usb_magic_t;

void kvm_usb_magic_init(kvm_usb_magic_t *m, const char *pattern, size_t length);

/** Feeds bytes; returns true once on a full match and then restarts. A 0 byte
 *  mid-pattern restarts the match at 1, as the diagnostics matcher does. */
bool kvm_usb_magic_feed(kvm_usb_magic_t *m, const uint8_t *bytes, size_t n);

#endif
