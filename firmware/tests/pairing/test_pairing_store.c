/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks that opaque bond tokens survive an NVS reload and that corrupted
 * metadata fails closed without erasing or silently replacing any bond. */
#include <assert.h>
#include <string.h>
#include "hid_pairing_store.h"
#include "nvs.h"

static unsigned char blob[256];
static size_t blob_size;
static unsigned commits;
static const char *expected_namespace = "kvm_bonds";
static unsigned legacy_opens;
int nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)mode; assert(strcmp(name, expected_namespace) == 0);
  if (strcmp(name, "kvm_bonds") == 0) legacy_opens++; *handle = 1; return 0; }
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{ (void)handle; assert(strcmp(key, "identities") == 0);
  if (!blob_size) return ESP_ERR_NVS_NOT_FOUND;
  assert(*length >= blob_size); memcpy(value, blob, blob_size); *length = blob_size; return 0; }
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{ (void)handle; assert(strcmp(key, "identities") == 0);
  assert(length <= sizeof(blob)); memcpy(blob, value, length); blob_size = length; return 0; }
int nvs_commit(nvs_handle_t handle) { (void)handle; commits++; return 0; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

int main(void)
{
    hid_pairing_t original, reloaded;
    hid_pairing_init(&original);
    assert(hid_pairing_store_load(&reloaded) == ESP_OK && reloaded.bond_count == 0);
    hid_peer_t identity = {.type = 1, .address = {1, 2, 3, 4, 5, 6}};
    hid_token_t token = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    assert(hid_pairing_add_bond(&original, identity, token));
    assert(hid_pairing_store_save(&original) == ESP_OK && commits == 1);
    assert(hid_pairing_store_load(&reloaded) == ESP_OK);
    hid_token_t found;
    assert(reloaded.bond_count == 1 && hid_pairing_token(&reloaded, identity, &found));
    assert(memcmp(found.bytes, token.bytes, DOUBTECH_HID_TOKEN_LEN) == 0);
    blob[4] = 1; /* The unshipped 64-bit-token development format is rejected. */
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE);
    blob[4] = 2;
    blob[0] = 0;
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE);
    assert(commits == 1);
    /* A table written by the shipped firmware: little-endian magic 0x4b564d42,
       version 2, count 2, then two 23-byte bonds (type, address[6], token[16]). */
    static const unsigned char legacy_blob[196] = {
        0x42, 0x4d, 0x56, 0x4b, 0x02, 0, 0, 0, 0x02, 0, 0, 0,
        1, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
    };
    assert(hid_pairing_store_set_namespace(NULL) == ESP_OK);
    memset(blob, 0, sizeof(blob));
    memcpy(blob, legacy_blob, sizeof(legacy_blob));
    blob_size = sizeof(legacy_blob);
    assert(hid_pairing_store_load(&reloaded) == ESP_OK && reloaded.bond_count == 2);
    hid_peer_t first = {.type = 1, .address = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6}};
    hid_peer_t second = {.type = 0, .address = {0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6}};
    assert(hid_pairing_token(&reloaded, first, &found) && found.bytes[0] == 0x10 && found.bytes[15] == 0x1f);
    assert(hid_pairing_token(&reloaded, second, &found) && found.bytes[0] == 0x20 && found.bytes[15] == 0x2f);

    /* An override namespace never touches the legacy one. */
    legacy_opens = 0;
    expected_namespace = "dt_bonds";
    assert(hid_pairing_store_set_namespace("0123456789abcdef") == ESP_ERR_INVALID_ARG);
    assert(hid_pairing_store_set_namespace("") == ESP_ERR_INVALID_ARG);
    assert(hid_pairing_store_set_namespace("dt_bonds") == ESP_OK);
    blob_size = 0;
    unsigned commits_before = commits;
    assert(hid_pairing_store_load(&reloaded) == ESP_OK && reloaded.bond_count == 0);
    assert(hid_pairing_store_save(&original) == ESP_OK && commits == commits_before + 1);
    assert(hid_pairing_store_load(&reloaded) == ESP_OK && reloaded.bond_count == 1);
    blob[0] = 0;
    unsigned char corrupt[sizeof(blob)];
    memcpy(corrupt, blob, sizeof(blob));
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE);
    assert(blob_size != 0 && memcmp(corrupt, blob, sizeof(blob)) == 0 && commits == commits_before + 1);
    assert(legacy_opens == 0);
    assert(hid_pairing_store_set_namespace("0123456789abcdef") == ESP_ERR_INVALID_ARG);
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE && legacy_opens == 0);
    expected_namespace = "kvm_bonds";
    assert(hid_pairing_store_set_namespace(NULL) == ESP_OK);
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE && legacy_opens == 1);
    return 0;
}
