/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the pure USB HID sink for the PC output. Each stream (keyboard,
 * pointer, consumer) keeps a small ordered FIFO of distinct states, so a press
 * and its release that arrive together (one BLE connection event) or while the
 * endpoint is busy both reach the PC, in order. The USB worker pumps it
 * without blocking through a ready/send port; a busy or refused report stays
 * queued and is retried, and the newest state is always the last one sent, so
 * the PC can never be left holding a key. No TinyUSB or ESP-IDF headers. */
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
/** Five buttons are declared; the upper three bits are constant padding (masked on apply). */
#define KVM_USB_HID_BUTTON_MASK 0x1Fu
/** Ordered FIFO depths per stream. */
#define KVM_USB_HID_KEY_QUEUE_DEPTH 16u
#define KVM_USB_HID_POINTER_QUEUE_DEPTH 16u
#define KVM_USB_HID_CONSUMER_QUEUE_DEPTH 8u

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

/** One queued pointer report source: a button state and the not yet reported motion under
 * it (saturating accumulators, never wrap). transition marks an entry started by a button
 * change (or release_all); it takes only the motion of the input that changed the buttons,
 * and later motion under the same buttons goes to a following motion-only entry. */
typedef struct {
    uint8_t buttons;
    bool transition;
    int32_t dx, dy;
    int16_t wheel, pan;
} kvm_usb_hid_pointer_entry_t;

/** Sink state; public for static allocation, serialized by the USB worker.
 * keys, buttons and consumer are each stream's newest state: the last queued state, or the
 * last sent one when the stream's queue is empty. apply queues a state only when it differs
 * from that newest state; release_all queues one all-zero state per stream unconditionally.
 * Each queue is a ring (head = oldest, count entries); an entry is removed only after the
 * port's send returned true (a pointer entry only once all its motion is reported).
 * Queue full: the NEWEST entry is replaced (pointer: its buttons replaced, motion added), so
 * states are never reordered and the final state is never dropped; only then can an
 * intermediate press/release pair merge away.
 * consumer_next picks the consumer report first on interface 1 when both streams have
 * entries; after every send attempt on interface 1 it prefers the other stream, so neither
 * starves the other on the shared endpoint. */
typedef struct {
    uint8_t keys[8];
    uint8_t buttons;
    uint16_t consumer;
    uint8_t key_queue[KVM_USB_HID_KEY_QUEUE_DEPTH][8];
    uint8_t key_head, key_count;
    kvm_usb_hid_pointer_entry_t pointer_queue[KVM_USB_HID_POINTER_QUEUE_DEPTH];
    uint8_t pointer_head, pointer_count;
    uint16_t consumer_queue[KVM_USB_HID_CONSUMER_QUEUE_DEPTH];
    uint8_t consumer_head, consumer_count;
    bool consumer_next;
} kvm_usb_hid_sink_t;

/** Zeroes all state, all queues empty, consumer_next true. */
void kvm_usb_hid_sink_init(kvm_usb_hid_sink_t *s);

/** Applies one router input. Keyboard (all 8 bytes, verbatim) and consumer queue the new
 * state when it differs from the newest. Pointer: a button change queues a transition entry
 * carrying this input's motion; motion under unchanged buttons is added to the newest
 * motion-only entry (or starts one), so motion coalesces only within one button state and a
 * press, the motion while held, and the release are reported in order. Unknown kinds and NULL
 * arguments are ignored. */
void kvm_usb_hid_sink_apply(kvm_usb_hid_sink_t *s, const kvm_router_input_t *in);

/** Drops everything queued (including pending motion), then queues exactly one all-zero state
 * per stream, even if the PC was already believed to be all-up. */
void kvm_usb_hid_sink_release_all(kvm_usb_hid_sink_t *s);

/** Zeroes everything and empties all queues (same as init): the host already dropped its
 * state (bus reset, re-enumeration, unmount), so nothing must be emitted. */
void kvm_usb_hid_sink_abandon(kvm_usb_hid_sink_t *s);

/** Writes the next boot keyboard report: the oldest queued state, or the newest state when
 * nothing is queued; 8 bytes (modifiers, reserved, six usages) exactly as applied. Bytes pass
 * through unchanged, including the 0x01 ErrorRollOver "phantom" usage a source reports on
 * overflow: the host then keeps its previous key state until the next normal report, and
 * release_all always queues a real all-up. */
void kvm_usb_hid_keyboard_report(const kvm_usb_hid_sink_t *s, uint8_t out[8]);

/** Builds one mouse report (Report ID 1 payload, little endian) from the oldest pointer entry:
 *   out[0]   buttons (bit0 left, bit1 right, bit2 middle, bit3/4 back/forward; bits 5-7 zero)
 *   out[1:2] X, int16, clamped to +-32767
 *   out[3:4] Y, int16, clamped to +-32767
 *   out[5]   wheel, int8, clamped to +-127
 *   out[6]   AC Pan, int8, clamped to +-127
 * and consumes it: the chunk is subtracted from the entry, which is removed once no motion
 * remains (a zero-motion transition is removed after its single report). Returns false (out
 * zeroed) when no pointer entry is queued. The pump builds the same report but commits it
 * only after send returned true. */
bool kvm_usb_hid_pointer_report(kvm_usb_hid_sink_t *s, uint8_t out[7]);

/** Writes the next consumer report (Report ID 2 payload): the oldest queued usage, or the
 * newest usage when nothing is queued, little endian. */
void kvm_usb_hid_consumer_report(const kvm_usb_hid_sink_t *s, uint8_t out[2]);

/** Sends at most one report per interface without blocking: interface 0 the oldest keyboard
 * state; interface 1 either the oldest consumer usage or one chunk of the oldest pointer
 * entry (alternating when both are queued). An interface whose ready() is false is skipped.
 * A report whose send() returns false leaves its entry queued unchanged, so the same entry is
 * retried. Returns true when nothing is pending afterwards. */
bool kvm_usb_hid_sink_pump(kvm_usb_hid_sink_t *s, const kvm_usb_hid_port_t *port);

/** True when any stream has a queued entry. */
bool kvm_usb_hid_sink_pending(const kvm_usb_hid_sink_t *s);

#endif
