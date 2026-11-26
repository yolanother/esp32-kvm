/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the pure, state-based USB HID sink for the PC output. The sink
 * holds the latest keyboard, pointer and consumer state (not an event queue);
 * the USB worker pumps it without blocking through a ready/send port, so a
 * busy or refused report is simply resent later with whatever state is newest
 * and the PC can never be left holding a key. No TinyUSB or ESP-IDF headers. */
#ifndef ESP32_KVM_USB_HID_SINK_H
#define ESP32_KVM_USB_HID_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "router.h"

/** Report payload lengths, excluding the report ID byte. */
#define KVM_USB_HID_KEYBOARD_REPORT_LEN 8u
#define KVM_USB_HID_POINTER_REPORT_LEN 7u
#define KVM_USB_HID_CONSUMER_REPORT_LEN 2u
/** Logical limits of the Task 2 pointer descriptor (X/Y 16-bit, wheel/pan 8-bit). */
#define KVM_USB_HID_AXIS_MAX 32767
#define KVM_USB_HID_WHEEL_MAX 127
/** Five buttons are declared; the upper three bits are constant padding. */
#define KVM_USB_HID_BUTTON_MASK 0x1Fu

/** Non-blocking HID port. ready(itf) mirrors tud_hid_n_ready; send queues one report and
 * returns false when it was not accepted (nothing was sent). report_id is passed separately
 * and is NOT part of data: 0 for the keyboard (interface 0 has no report ID),
 * KVM_USB_REPORT_ID_MOUSE or KVM_USB_REPORT_ID_CONSUMER on interface 1. A NULL ready is
 * treated as always ready; a NULL port or send sends nothing. */
typedef struct {
    bool (*ready)(void *context, uint8_t itf);
    bool (*send)(void *context, uint8_t itf, uint8_t report_id, const uint8_t *data, size_t len);
    void *context;
} kvm_usb_hid_port_t;

/** Sink state; public for static allocation, serialized by the USB worker.
 * keys is the latest 8-byte boot keyboard state; buttons the latest pointer buttons;
 * dx/dy/wheel/pan the motion not yet reported (saturating accumulators, never wrap);
 * consumer the latest consumer usage (0 = released).
 * *_dirty means a report carrying the latest state is owed to the PC. Every apply owes one
 * report of its kind (an identical state is resent, which is harmless and idempotent).
 * pointer_dirty is also true whenever unreported motion remains. A flag is cleared only after
 * the port's send returned true. consumer_next picks the consumer report first on
 * interface 1 when both consumer and pointer are owed; after every send attempt on
 * interface 1 it is set to prefer the other kind, so neither report kind starves the other on
 * the shared endpoint (even while new motion arrives before every pump).
 *
 * Coalescing (state-based by design): states applied between two successful sends collapse
 * into the newest one, e.g. a consumer press and release applied while the endpoint is busy
 * deliver only the release; a key or button press that is applied and released before it
 * could be sent is never seen by the PC. The release is never lost. */
typedef struct {
    uint8_t keys[8];
    uint8_t buttons;
    int32_t dx, dy;
    int16_t wheel, pan;
    uint16_t consumer;
    bool keys_dirty, pointer_dirty, consumer_dirty;
    bool consumer_next;
} kvm_usb_hid_sink_t;

/** Zeroes all state, nothing owed, consumer_next true. */
void kvm_usb_hid_sink_init(kvm_usb_hid_sink_t *s);

/** Applies one router input. Keyboard replaces keys (all 8 bytes, verbatim) and consumer
 * replaces the usage; pointer replaces buttons and adds dx/dy/wheel/pan to the saturating
 * accumulators. Marks that kind's report owed. Unknown kinds and NULL arguments are ignored. */
void kvm_usb_hid_sink_apply(kvm_usb_hid_sink_t *s, const kvm_router_input_t *in);

/** Zeroes keys, buttons and consumer, drops pending motion, and marks all three reports owed,
 * so all-up reports go out even if the PC was already believed to be all-up. */
void kvm_usb_hid_sink_release_all(kvm_usb_hid_sink_t *s);

/** Zeroes everything with nothing owed (same as init): the host already dropped its state
 * (bus reset, re-enumeration, unmount), so nothing must be emitted. */
void kvm_usb_hid_sink_abandon(kvm_usb_hid_sink_t *s);

/** Writes the boot keyboard report: exactly the applied input's 8 bytes (modifiers, reserved,
 * six usages). Bytes are passed through unchanged, including the 0x01 ErrorRollOver
 * "phantom" usage a source reports on overflow: the host then keeps its previous key state
 * until the next normal report, and release_all always sends a real all-up. */
void kvm_usb_hid_keyboard_report(const kvm_usb_hid_sink_t *s, uint8_t out[8]);

/** Builds one mouse report (Report ID 1 payload, little endian):
 *   out[0]   buttons & KVM_USB_HID_BUTTON_MASK (bit0 left, bit1 right, bit2 middle, bit3/4 back/forward)
 *   out[1:2] X, int16, clamped to +-32767
 *   out[3:4] Y, int16, clamped to +-32767
 *   out[5]   wheel, int8, clamped to +-127
 *   out[6]   AC Pan, int8, clamped to +-127
 * The emitted chunk is subtracted from the accumulators; any remainder keeps pointer_dirty set.
 * Buttons are always the current (newest) state, so a release travels with the next motion
 * chunk and is never reordered after it. Returns false (out zeroed, state unchanged) when no
 * pointer report is owed. */
bool kvm_usb_hid_pointer_report(kvm_usb_hid_sink_t *s, uint8_t out[7]);

/** Writes the consumer report (Report ID 2 payload): the 16-bit usage, little endian. */
void kvm_usb_hid_consumer_report(const kvm_usb_hid_sink_t *s, uint8_t out[2]);

/** Sends at most one report per interface without blocking: interface 0 the keyboard when
 * owed; interface 1 either the consumer or one pointer chunk (alternating when both are owed).
 * An interface whose ready() is false is skipped. A report whose send() returns false leaves
 * the sink unchanged (no motion consumed). Returns true when nothing is pending afterwards. */
bool kvm_usb_hid_sink_pump(kvm_usb_hid_sink_t *s, const kvm_usb_hid_port_t *port);

/** True when any report is owed or unreported motion remains. */
bool kvm_usb_hid_sink_pending(const kvm_usb_hid_sink_t *s);

#endif
