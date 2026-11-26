/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure USB boot-mode decision (spec Decision 7) and the magic
 * string matcher. Deliberately free of ESP-IDF includes so it runs on the host. */
#include "usb_mode.h"
#include <string.h>

static kvm_usb_boot_decision_t serial(kvm_usb_reason_t reason)
{
    kvm_usb_boot_decision_t d = { KVM_USB_MODE_SERIAL_JTAG, reason, false, false };
    return d;
}

kvm_usb_boot_decision_t kvm_usb_mode_decide(const kvm_usb_boot_inputs_t *in)
{
    if (in == NULL) return serial(KVM_USB_REASON_DEFAULT);
    if (!in->composite_built) return serial(KVM_USB_REASON_NOT_BUILT);
    if (in->force_serial) return serial(KVM_USB_REASON_FORCED_BY_BUILD);
    if (in->serial_once) return serial(KVM_USB_REASON_SERIAL_ONCE);
    if (in->escape_held) return serial(KVM_USB_REASON_ESCAPE_BUTTON);
    if (in->abnormal_reset) return serial(KVM_USB_REASON_ABNORMAL_RESET);
    if (in->unconfirmed >= in->max_unconfirmed) {
        kvm_usb_boot_decision_t d = serial(KVM_USB_REASON_UNCONFIRMED_LIMIT);
        d.persist_serial = true;
        return d;
    }
    if (in->stored_mode == 1) {
        kvm_usb_boot_decision_t d = { KVM_USB_MODE_COMPOSITE, KVM_USB_REASON_STORED, false, true };
        return d;
    }
    return serial(KVM_USB_REASON_DEFAULT);
}

/* Single source of truth: the decision itself, with the escape button up. */
bool kvm_usb_mode_needs_window(const kvm_usb_boot_inputs_t *in)
{
    if (in == NULL) return false;
    kvm_usb_boot_inputs_t no_escape = *in;
    no_escape.escape_held = false;
    return kvm_usb_mode_decide(&no_escape).mode == KVM_USB_MODE_COMPOSITE;
}

void kvm_usb_escape_init(kvm_usb_escape_t *e, uint32_t window_ms, uint32_t hold_ms)
{
    if (e == NULL) return;
    e->window_ms = window_ms;
    e->hold_ms = hold_ms;
    e->down = false;
    e->down_since = 0;
    e->held = false;
}

bool kvm_usb_escape_sample(kvm_usb_escape_t *e, bool boot_down, uint32_t elapsed_ms)
{
    if (e == NULL || e->held || elapsed_ms >= e->window_ms) return false;
    if (!boot_down) {
        e->down = false;
        return true;
    }
    if (!e->down) {
        e->down = true;
        e->down_since = elapsed_ms;
    }
    if (elapsed_ms - e->down_since >= e->hold_ms) e->held = true;
    return !e->held;
}

void kvm_usb_magic_init(kvm_usb_magic_t *m, const char *pattern, size_t length)
{
    if (m == NULL) return;
    m->pattern = pattern;
    m->length = length;
    m->matched = 0;
}

/* After pattern[0..k) matched and byte b did not extend it, the bytes seen end
 * with pattern[0..k) + b. Returns the longest j <= k such that pattern[0..j)
 * is a suffix of that text (the KMP fallback, computed without a table). */
static size_t fallback(const char *p, size_t k, uint8_t b)
{
    for (size_t j = k; j > 0; --j) {
        if ((uint8_t)p[j - 1] != b) continue;
        if (j == 1 || memcmp(p, p + (k - j + 1), j - 1) == 0) return j;
    }
    return 0;
}

bool kvm_usb_magic_feed(kvm_usb_magic_t *m, const uint8_t *bytes, size_t n)
{
    if (m == NULL || m->pattern == NULL || m->length == 0 || bytes == NULL) return false;
    if (m->matched >= m->length) m->matched = 0;
    for (size_t i = 0; i < n; ++i) {
        if (bytes[i] == (uint8_t)m->pattern[m->matched]) ++m->matched;
        else m->matched = fallback(m->pattern, m->matched, bytes[i]);
        if (m->matched == m->length) {
            m->matched = 0;
            return true;
        }
    }
    return false;
}
