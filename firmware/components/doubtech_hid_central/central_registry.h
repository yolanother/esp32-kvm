/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Remembers at most one paired input device per kind (keyboard, mouse).
 *
 * Same-address policy: slots are keyed by kind only. A combo device may be
 * remembered as both a keyboard and a mouse with the same address; the two
 * records are independent. Setting a kind replaces only that kind's slot and
 * forgetting one kind never touches the other. */
#ifndef DTHID_CENTRAL_REGISTRY_H
#define DTHID_CENTRAL_REGISTRY_H
#include <stdbool.h>
#include <stdint.h>

#define DTHID_INPUT_NAME_BYTES 24

typedef enum { DTHID_INPUT_KEYBOARD = 1, DTHID_INPUT_MOUSE = 2 } dthid_input_kind_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    dthid_input_kind_t kind;
    char name[DTHID_INPUT_NAME_BYTES];
} dthid_input_record_t;

typedef struct {
    dthid_input_record_t slots[2]; /* index is kind - 1 */
    bool used[2];
} central_registry_t;

/* Stores rec in its kind's slot, replacing any earlier record of that kind.
 * The name is copied bounded and always NUL-terminated. Returns false for a
 * NULL argument or an unknown kind, leaving the registry unchanged. */
bool central_registry_set(central_registry_t *r, const dthid_input_record_t *rec);
bool central_registry_get(const central_registry_t *r, dthid_input_kind_t kind, dthid_input_record_t *out);
/* Returns true when a record of that kind existed and was removed. */
bool central_registry_forget(central_registry_t *r, dthid_input_kind_t kind);

#endif
