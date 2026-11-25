/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Pure helpers for the input pairing scan: a bounded advertising-data reader
 * and a deduplicated list of at most eight HID keyboard or mouse candidates.
 * A device is listed when it advertises the HID service UUID 0x1812 or the
 * keyboard (0x03C1), mouse (0x03C2) or gamepad (0x03C4) appearance. Rows keep
 * their index for the whole window so a selection by index stays stable; the
 * display sorts them (for example by RSSI). */
#ifndef DTHID_CENTRAL_SCAN_H
#define DTHID_CENTRAL_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doubtech_hid_central.h"

#define CENTRAL_SCAN_MAX DOUBTECH_HID_CENTRAL_MAX_SCAN_RESULTS
#define CENTRAL_SCAN_NAME_BYTES 24u
#define CENTRAL_APPEARANCE_KEYBOARD 0x03C1u
#define CENTRAL_APPEARANCE_MOUSE 0x03C2u
#define CENTRAL_APPEARANCE_GAMEPAD 0x03C4u /* listed (spec filter), kind unknown */

typedef struct {
    bool hid_service;
    bool has_appearance;
    uint16_t appearance;
    char name[CENTRAL_SCAN_NAME_BYTES]; /* empty when absent */
} central_adv_info_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t kind; /* 0 unknown */
    int8_t rssi;
    char name[CENTRAL_SCAN_NAME_BYTES];
} central_scan_entry_t;

typedef struct {
    central_scan_entry_t entries[CENTRAL_SCAN_MAX];
    uint8_t count;
} central_scan_list_t;

/** Reads AD structures (16-bit UUID lists, appearance, short or complete name).
 * Stops at the first malformed structure and keeps what was read before it.
 * Names are truncated to 23 bytes; control bytes become '?'. */
void central_adv_parse(const uint8_t *data, size_t len, central_adv_info_t *out);

void central_scan_clear(central_scan_list_t *list);
/** Offers one advertising report. Returns the row index when a row was added or
 * its name or kind became known (report it), otherwise -1 (filtered out, list
 * full, or only the RSSI changed). */
int central_scan_offer(central_scan_list_t *list, const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                       const central_adv_info_t *info);

/** Whether a scan row may be paired: 0 when it may, else reason 5 (capacity)
 * when the address is a guest identity (never connect or bond a guest), already
 * stored for any kind (forget first), or its advertised kind has no free slot.
 * free_kinds holds bit (1 << kind) for each kind not yet stored. */
uint8_t central_scan_select_reason(const central_scan_entry_t *entry, uint8_t free_kinds, bool stored_peer,
                                   bool guest_peer);
/** A NimBLE bond may be deleted only when its peer is neither a guest nor a
 * stored input device. The guest and central bonds share one key store. */
bool central_bond_deletable(bool guest_peer, bool stored_peer);

#endif
