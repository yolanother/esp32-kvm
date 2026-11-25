/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * NVS persistence for the input-device registry. The blob is written and then
 * committed, so a failed save leaves the previously committed blob intact. */
#include "central_store.h"
#include <string.h>
#include "nvs.h"

#ifndef ESP_ERR_NVS_INVALID_LENGTH
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#endif

#define STORE_MAGIC 0x49504448u /* "HDPI" */
#define STORE_VERSION 1u
#define STORE_KEY "inputs"
#define GUEST_NAMESPACE "kvm_bonds"
#define HEADER_BYTES 16u
#define RECORD_BYTES (6u + 1u + 1u + DTHID_INPUT_NAME_BYTES)
#define MAX_RECORDS 2u
#define MAX_BLOB_BYTES (HEADER_BYTES + MAX_RECORDS * RECORD_BYTES)

static uint32_t crc32_of(const uint8_t *data, size_t length, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t check_namespace(const char **ns)
{
    if (*ns == NULL) *ns = DTHID_INPUTS_DEFAULT_NAMESPACE;
    size_t length = strlen(*ns);
    if (length == 0 || length > DTHID_INPUTS_NAMESPACE_MAX_CHARS) return ESP_ERR_INVALID_ARG;
    if (strcmp(*ns, GUEST_NAMESPACE) == 0) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

/* CRC covers the header fields before the crc word and every record byte. */
static uint32_t blob_crc(const uint8_t *blob, uint32_t count)
{
    uint32_t crc = crc32_of(blob, 12, 0);
    return crc32_of(blob + HEADER_BYTES, count * RECORD_BYTES, crc);
}

/* Decodes a blob into out; false means the blob is not a valid registry. */
static bool decode(const uint8_t *blob, size_t size, central_registry_t *out)
{
    if (size < HEADER_BYTES || size > MAX_BLOB_BYTES) return false;
    if (get32(blob) != STORE_MAGIC || get32(blob + 4) != STORE_VERSION) return false;
    uint32_t count = get32(blob + 8);
    if (count > MAX_RECORDS || size != HEADER_BYTES + count * RECORD_BYTES) return false;
    if (get32(blob + 12) != blob_crc(blob, count)) return false;
    central_registry_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *src = blob + HEADER_BYTES + i * RECORD_BYTES;
        dthid_input_record_t rec;
        memset(&rec, 0, sizeof(rec));
        memcpy(rec.addr, src, 6);
        rec.addr_type = src[6];
        rec.kind = (dthid_input_kind_t)src[7];
        if (memchr(src + 8, '\0', DTHID_INPUT_NAME_BYTES) == NULL) return false;
        memcpy(rec.name, src + 8, DTHID_INPUT_NAME_BYTES);
        if (rec.kind != DTHID_INPUT_KEYBOARD && rec.kind != DTHID_INPUT_MOUSE) return false;
        if (parsed.used[rec.kind - 1]) return false; /* duplicate kind */
        if (!central_registry_set(&parsed, &rec)) return false;
    }
    *out = parsed;
    return true;
}

esp_err_t central_store_load(const char *ns, central_registry_t *r)
{
    if (r == NULL) return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof(*r));
    esp_err_t result = check_namespace(&ns);
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    result = nvs_open(ns, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    uint8_t blob[MAX_BLOB_BYTES];
    size_t size = sizeof(blob);
    result = nvs_get_blob(handle, STORE_KEY, blob, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND || result == ESP_ERR_NVS_INVALID_LENGTH) return ESP_OK;
    if (result != ESP_OK) return result;
    central_registry_t parsed;
    if (decode(blob, size, &parsed)) *r = parsed;
    return ESP_OK;
}

esp_err_t central_store_save(const char *ns, const central_registry_t *r)
{
    if (r == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = check_namespace(&ns);
    if (result != ESP_OK) return result;
    uint8_t blob[MAX_BLOB_BYTES];
    memset(blob, 0, sizeof(blob));
    uint32_t count = 0;
    for (int slot = 0; slot < 2; ++slot) {
        if (!r->used[slot]) continue;
        const dthid_input_record_t *rec = &r->slots[slot];
        uint8_t *dst = blob + HEADER_BYTES + count * RECORD_BYTES;
        memcpy(dst, rec->addr, 6);
        dst[6] = rec->addr_type;
        dst[7] = (uint8_t)(slot + 1);
        size_t length = 0;
        while (length < DTHID_INPUT_NAME_BYTES - 1 && rec->name[length] != '\0') ++length;
        memcpy(dst + 8, rec->name, length);
        ++count;
    }
    put32(blob, STORE_MAGIC);
    put32(blob + 4, STORE_VERSION);
    put32(blob + 8, count);
    put32(blob + 12, blob_crc(blob, count));
    nvs_handle_t handle;
    result = nvs_open(ns, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, STORE_KEY, blob, HEADER_BYTES + count * RECORD_BYTES);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
