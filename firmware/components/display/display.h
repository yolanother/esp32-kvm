/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the device status screen and runtime button sampler. The panel is
 * opt-in through the validated Waveshare board profile; BOOT emergency sampling
 * remains available while the panel is disabled or its startup fails. */
#ifndef ESP32_KVM_DISPLAY_H
#define ESP32_KVM_DISPLAY_H

#include "display_model.h"
#include "esp_err.h"

/** Delivers a debounced physical button request to the serialized router worker. */
typedef void (*kvm_display_event_fn)(kvm_display_event_t event);
/** Delivers a validated exact touch action to the serialized USB worker. */
typedef void (*kvm_display_pair_fn)(kvm_display_pair_request_t request);
/** Queues one visible target card selection; zero is the local host. */
typedef void (*kvm_display_target_fn)(uint8_t slot);

/** Delivers a validated input-device pairing touch to the serialized USB worker. */
typedef void (*kvm_display_input_fn)(kvm_display_input_request_t request);

/** Sets the input-device pairing touch consumer; call before kvm_display_start. */
void kvm_display_set_input_fn(kvm_display_input_fn input_fn);

/** Delivers a confirmed USB mode screen request to the serialized USB worker;
 *  must not block (the display task calls it). */
typedef void (*kvm_display_usb_fn)(kvm_display_usb_request_t request);
/** Sets the USB mode request consumer; call before kvm_display_start. The
 *  display never calls USB or boot code itself; without a consumer the
 *  screen's entry row does nothing. */
void kvm_display_set_usb_fn(void (*fn)(kvm_display_usb_request_t));
/** Starts BOOT sampling, then optionally the validated board LCD and touch. */
esp_err_t kvm_display_start(bool enable_panel, kvm_display_event_fn event_fn,
                            kvm_display_pair_fn pair_fn, kvm_display_target_fn target_fn);
/** Copies the latest routing status for the display task without blocking the router. */
void kvm_display_post_status(const kvm_display_status_t *status);
/** Smallest amount of free display task stack seen so far, in bytes (0 when the panel task is not running). */
uint32_t kvm_display_stack_high_water_bytes(void);

#endif
