/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the serialized firmware status projection for the device display.
 * It copies confirmed USB, router, pairing, and authenticated HID facts;
 * unavailable update, touch, and panel sources remain disabled. Also the pure
 * worker side of USB mode requests from the screen (queue and decision). */
#ifndef ESP32_KVM_TRANSPORT_DISPLAY_STATUS_H
#define ESP32_KVM_TRANSPORT_DISPLAY_STATUS_H

#include "transport_core.h"
#include "display_model.h"

/** Builds one fail-closed display status on the USB worker at monotonic now_ms.
 *  The USB mode fields are left 0 (today's serial screen); the worker adds
 *  them afterwards with kvm_transport_display_usb. */
void kvm_transport_display_status(const kvm_transport_core_t *core,
                                  const kvm_router_t *router, bool usb_connected,
                                  bool hid_ready, uint64_t now_ms,
                                  kvm_display_status_t *status);

/** Copies this boot's USB facts into the snapshot: usb_mode 1 only for mode 1
 *  (kvm_usb_mode_t COMPOSITE), anything else 0; usb_reason as given
 *  (kvm_usb_reason_t); pc_hid (0 not enumerated, 1 suspended, 2 ready) only in
 *  composite mode, out of range as 0. Serial mode has no PC HID: always 0. */
void kvm_transport_display_usb(uint8_t mode, uint8_t reason, uint8_t pc_hid,
                               kvm_display_status_t *status);

/* ---- USB mode requests from the screen (the worker's side) ---- */

/** Distinct request values; with repeats coalesced the queue never overflows. */
#define KVM_TRANSPORT_USB_QUEUE_DEPTH 3u

/** Bounded FIFO between the display task and the worker; the caller locks. */
typedef struct {
    uint8_t items[KVM_TRANSPORT_USB_QUEUE_DEPTH];
    uint8_t count;
} kvm_transport_usb_queue_t;

/** Appends a request. A repeat drops its older copy first (the newest tap
 *  wins its place), so a different pending request is never dropped. False
 *  for a value outside kvm_display_usb_request_t or a NULL queue. */
bool kvm_transport_usb_queue_push(kvm_transport_usb_queue_t *queue, kvm_display_usb_request_t request);
/** Removes the oldest request; false when empty. */
bool kvm_transport_usb_queue_pop(kvm_transport_usb_queue_t *queue, kvm_display_usb_request_t *request);

/** What the worker does with a request. */
typedef enum {
    KVM_TRANSPORT_USB_ACTION_NONE,           /* not a request: ignored */
    KVM_TRANSPORT_USB_ACTION_NOT_BUILT,      /* composite on an image without it: refused, nothing touched */
    KVM_TRANSPORT_USB_ACTION_BUSY,           /* a guest is armed or a guest window is open: refused */
    KVM_TRANSPORT_USB_ACTION_SERIAL_ONCE,    /* release, then kvm_usb_reboot_serial_once() */
    KVM_TRANSPORT_USB_ACTION_SERIAL_MODE,    /* release, then kvm_usb_reboot_with_mode(SERIAL) */
    KVM_TRANSPORT_USB_ACTION_COMPOSITE_MODE  /* release, then kvm_usb_reboot_with_mode(COMPOSITE) */
} kvm_transport_usb_action_t;

/** Decides a request: composite without build support is NOT_BUILT (checked
 *  first, so it never releases anything); a restart is BUSY while host_armed
 *  or guest_pairing, the same gate as the screen's. */
kvm_transport_usb_action_t kvm_transport_usb_action(kvm_display_usb_request_t request,
                                                    bool composite_supported,
                                                    bool host_armed, bool guest_pairing);
/** The KVM_DISPLAY_USB_RESULT_* the screen shows for an action. */
uint8_t kvm_transport_usb_action_result(kvm_transport_usb_action_t action);
/** True for the actions that release everything and restart. */
bool kvm_transport_usb_action_reboots(kvm_transport_usb_action_t action);

#endif
