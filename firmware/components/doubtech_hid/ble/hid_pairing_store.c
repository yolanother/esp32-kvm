/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Persists a versioned, bounded identity-to-opaque-token table in NVS. It
 * rejects unknown or corrupt data and never erases bonds during recovery. */
#include "hid_pairing_store.h"
#include <string.h>
#include "nvs.h"

#define TABLE_MAGIC 0x4b564d42u
#define TABLE_VERSION 2u
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    hid_bond_t bonds[HID_PAIRING_MAX_BONDS];
} stored_table_t;

esp_err_t hid_pairing_store_load(hid_pairing_t *state)
{
    nvs_handle_t handle;
    hid_pairing_init(state);
    esp_err_t result = nvs_open("kvm_bonds", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    stored_table_t table = {0};
    size_t size = sizeof(table);
    result = nvs_get_blob(handle, "identities", &table, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    if (size != sizeof(table) || table.magic != TABLE_MAGIC ||
        table.version != TABLE_VERSION || table.count > HID_PAIRING_MAX_BONDS)
        return ESP_ERR_INVALID_STATE;
    for (uint32_t index = 0; index < table.count; ++index)
        if (!hid_pairing_add_bond(state, table.bonds[index].peer, table.bonds[index].token))
            return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t hid_pairing_store_save(const hid_pairing_t *state)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("kvm_bonds", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    stored_table_t table = {.magic = TABLE_MAGIC, .version = TABLE_VERSION,
                            .count = (uint32_t)state->bond_count};
    memcpy(table.bonds, state->bonds, state->bond_count * sizeof(hid_bond_t));
    result = nvs_set_blob(handle, "identities", &table, sizeof(table));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
