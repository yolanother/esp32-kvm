/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE. */
#ifndef ESP32_KVM_DISPLAY_LABELS_H
#define ESP32_KVM_DISPLAY_LABELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define KVM_DISPLAY_LABEL_BYTES 24u

/** Loads the versioned token-to-label table from NVS. */
esp_err_t kvm_display_labels_init(void);
/** Stores one bounded UTF-8 label and commits it before returning success. */
bool kvm_display_label_set(const uint8_t token[16], const char *label, size_t length);
/** Copies a label for a token; returns false when no label is stored. */
bool kvm_display_label_get(const uint8_t token[16], char out[KVM_DISPLAY_LABEL_BYTES + 1]);

/** Hides or shows a guest on the device. RAM only: the host resends this on every connect. */
bool kvm_display_guest_hidden_set(const uint8_t token[16], bool hidden);
/** True while the host has temporarily disabled the guest with this token. */
bool kvm_display_guest_hidden(const uint8_t token[16]);

#endif
