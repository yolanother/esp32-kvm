/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * In-memory registry of remembered input devices, one slot per kind. */
#include "central_registry.h"
#include <string.h>

static int slot_of(dthid_input_kind_t kind)
{
    if (kind == DTHID_INPUT_KEYBOARD) return 0;
    if (kind == DTHID_INPUT_MOUSE) return 1;
    return -1;
}

bool central_registry_set(central_registry_t *r, const dthid_input_record_t *rec)
{
    if (r == NULL || rec == NULL) return false;
    int slot = slot_of(rec->kind);
    if (slot < 0) return false;
    dthid_input_record_t *dst = &r->slots[slot];
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->addr, rec->addr, sizeof(dst->addr));
    dst->addr_type = rec->addr_type;
    dst->kind = rec->kind;
    size_t length = 0;
    while (length < DTHID_INPUT_NAME_BYTES - 1 && rec->name[length] != '\0') ++length;
    memcpy(dst->name, rec->name, length);
    dst->name[length] = '\0';
    r->used[slot] = true;
    return true;
}

bool central_registry_get(const central_registry_t *r, dthid_input_kind_t kind, dthid_input_record_t *out)
{
    if (r == NULL || out == NULL) return false;
    int slot = slot_of(kind);
    if (slot < 0 || !r->used[slot]) return false;
    *out = r->slots[slot];
    return true;
}

bool central_registry_forget(central_registry_t *r, dthid_input_kind_t kind)
{
    if (r == NULL) return false;
    int slot = slot_of(kind);
    if (slot < 0 || !r->used[slot]) return false;
    memset(&r->slots[slot], 0, sizeof(r->slots[slot]));
    r->used[slot] = false;
    return true;
}
