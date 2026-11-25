/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Persists the input-device registry as one versioned, CRC-protected NVS blob.
 *
 * Blob layout (little endian, 16-byte header then count * 32-byte records):
 *   u32 magic, u32 version, u32 count, u32 crc32 over magic..count and records.
 *   record: addr[6], addr_type, kind, name[24] (NUL-terminated).
 * A missing, truncated, oversized, corrupt, or unknown-version blob loads as an
 * empty registry and returns ESP_OK so a bad record never fails the boot. Only
 * real storage errors are returned. */
#ifndef DTHID_CENTRAL_STORE_H
#define DTHID_CENTRAL_STORE_H
#include "central_registry.h"
#include "esp_err.h"

#define DTHID_INPUTS_DEFAULT_NAMESPACE "dthid_inputs"
#define DTHID_INPUTS_NAMESPACE_MAX_CHARS 15

/* ns NULL selects the default. An empty name, one over 15 characters, or the
 * guest bond namespace returns ESP_ERR_INVALID_ARG; saving never touches it. */
esp_err_t central_store_load(const char *ns, central_registry_t *r);
esp_err_t central_store_save(const char *ns, const central_registry_t *r);

#endif
