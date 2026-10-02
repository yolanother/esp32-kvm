/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the device status screen and runtime button sampler. The panel is
 * opt-in until board pins and orientation are verified; BOOT emergency sampling
 * remains available while the panel is disabled. */
#ifndef ESP32_KVM_DISPLAY_H
#define ESP32_KVM_DISPLAY_H

#include "display_model.h"
#include "esp_err.h"

/** Delivers a debounced physical button request to the serialized router worker. */
typedef void (*kvm_display_event_fn)(kvm_display_event_t event);

/** Starts BOOT sampling and optionally the board LCD; PLUS remains unassigned. */
esp_err_t kvm_display_start(bool enable_panel, kvm_display_event_fn event_fn);
/** Copies the latest routing status for the display task without blocking the router. */
void kvm_display_post_status(const kvm_display_status_t *status);

#endif
