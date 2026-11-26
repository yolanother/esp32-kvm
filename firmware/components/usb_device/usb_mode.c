/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure USB boot-mode decision (spec Decision 7) and the magic
 * string matcher. Deliberately free of ESP-IDF includes so it runs on the host. */
#include "usb_mode.h"

static kvm_usb_boot_decision_t serial(kvm_usb_reason_t reason)
{
    kvm_usb_boot_decision_t d = { KVM_USB_MODE_SERIAL_JTAG, reason, false, false };
    return d;
}

/* True when every row except the escape button would still pick composite. */
static bool composite_candidate(const kvm_usb_boot_inputs_t *in)
{
    return in->composite_built && !in->force_serial && !in->serial_once && !in->abnormal_reset
        && in->unconfirmed < in->max_unconfirmed && in->stored_mode == 1;
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

bool kvm_usb_mode_needs_window(const kvm_usb_boot_inputs_t *in)
{
    return in != NULL && composite_candidate(in);
}

void kvm_usb_magic_init(kvm_usb_magic_t *m, const char *pattern, size_t length)
{
    if (m == NULL) return;
    m->pattern = pattern;
    m->length = length;
    m->matched = 0;
}

bool kvm_usb_magic_feed(kvm_usb_magic_t *m, const uint8_t *bytes, size_t n)
{
    if (m == NULL || m->pattern == NULL || m->length == 0 || bytes == NULL) return false;
    for (size_t i = 0; i < n; ++i) {
        if (bytes[i] == (uint8_t)m->pattern[m->matched]) ++m->matched;
        else m->matched = bytes[i] == 0 ? 1 : 0;
        if (m->matched == m->length) {
            m->matched = 0;
            return true;
        }
    }
    return false;
}
