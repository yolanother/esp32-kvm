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
int nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)mode; assert(strcmp(name, "kvm_bonds") == 0); *handle = 1; return 0; }
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
    assert(hid_pairing_add_bond(&original, identity, 0x123456789ULL));
    assert(hid_pairing_store_save(&original) == ESP_OK && commits == 1);
    assert(hid_pairing_store_load(&reloaded) == ESP_OK);
    assert(reloaded.bond_count == 1 && hid_pairing_token(&reloaded, identity) == 0x123456789ULL);
    blob[0] = 0;
    assert(hid_pairing_store_load(&reloaded) == ESP_ERR_INVALID_STATE);
    assert(commits == 1);
    return 0;
}
