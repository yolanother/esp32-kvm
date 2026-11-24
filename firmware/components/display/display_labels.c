/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Persists only friendly labels keyed by opaque bond tokens; no BLE address or key is stored. */
#include "display_labels.h"
#include "nvs.h"
#include <string.h>

#define LABEL_TABLE_VERSION 1u
#define LABEL_TABLE_ENTRIES 8u

typedef struct __attribute__((packed)) {
    uint8_t used;
    uint8_t token[16];
    uint8_t length;
    char text[KVM_DISPLAY_LABEL_BYTES];
} label_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    label_entry_t entries[LABEL_TABLE_ENTRIES];
} label_table_t;

#define HIDDEN_ENTRIES 8u

static label_table_t table;
static uint8_t hidden_tokens[HIDDEN_ENTRIES][16];
static bool hidden_used[HIDDEN_ENTRIES];
static nvs_handle_t handle;
static bool initialized;

esp_err_t kvm_display_labels_init(void)
{
    if (initialized) return ESP_OK;
    esp_err_t result = nvs_open("kvm_labels", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    size_t size = sizeof(table);
    result = nvs_get_blob(handle, "table", &table, &size);
    if (result != ESP_OK || size != sizeof(table) || table.version != LABEL_TABLE_VERSION) {
        memset(&table, 0, sizeof(table));
        table.version = LABEL_TABLE_VERSION;
    }
    initialized = true;
    return ESP_OK;
}

bool kvm_display_label_set(const uint8_t token[16], const char *label, size_t length)
{
    if (!initialized || !token || !label || !length || length > KVM_DISPLAY_LABEL_BYTES) return false;
    bool nonzero = false;
    for (size_t i = 0; i < 16; ++i) nonzero |= token[i] != 0;
    if (!nonzero) return false;
    label_entry_t *entry = NULL;
    for (size_t i = 0; i < LABEL_TABLE_ENTRIES; ++i) {
        if (table.entries[i].used && memcmp(table.entries[i].token, token, 16) == 0) {
            entry = &table.entries[i];
            break;
        }
        if (!entry && !table.entries[i].used) entry = &table.entries[i];
    }
    if (!entry) return false;
    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    memcpy(entry->token, token, 16);
    entry->length = (uint8_t)length;
    memcpy(entry->text, label, length);
    return nvs_set_blob(handle, "table", &table, sizeof(table)) == ESP_OK &&
           nvs_commit(handle) == ESP_OK;
}

bool kvm_display_label_get(const uint8_t token[16], char out[KVM_DISPLAY_LABEL_BYTES + 1])
{
    if (!out) return false;
    out[0] = '\0';
    if (!initialized || !token) return false;
    for (size_t i = 0; i < LABEL_TABLE_ENTRIES; ++i) {
        const label_entry_t *entry = &table.entries[i];
        if (!entry->used || entry->length > KVM_DISPLAY_LABEL_BYTES ||
            memcmp(entry->token, token, 16) != 0) continue;
        memcpy(out, entry->text, entry->length);
        out[entry->length] = '\0';
        return true;
    }
    return false;
}


bool kvm_display_guest_hidden_set(const uint8_t token[16], bool hidden)
{
    if (!token) return false;
    bool nonzero = false;
    for (size_t i = 0; i < 16; ++i) nonzero |= token[i] != 0;
    if (!nonzero) return false;
    size_t free_slot = HIDDEN_ENTRIES;
    for (size_t i = 0; i < HIDDEN_ENTRIES; ++i) {
        if (hidden_used[i] && memcmp(hidden_tokens[i], token, 16) == 0) {
            hidden_used[i] = hidden;
            if (!hidden) memset(hidden_tokens[i], 0, 16);
            return true;
        }
        if (!hidden_used[i] && free_slot == HIDDEN_ENTRIES) free_slot = i;
    }
    if (!hidden) return true;
    if (free_slot == HIDDEN_ENTRIES) return false;
    memcpy(hidden_tokens[free_slot], token, 16);
    hidden_used[free_slot] = true;
    return true;
}

bool kvm_display_guest_hidden(const uint8_t token[16])
{
    if (!token) return false;
    for (size_t i = 0; i < HIDDEN_ENTRIES; ++i)
        if (hidden_used[i] && memcmp(hidden_tokens[i], token, 16) == 0) return true;
    return false;
}
