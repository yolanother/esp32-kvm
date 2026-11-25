/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Pure pairing-scan helpers (see central_scan.h): a bounded AD-structure reader
 * and the deduplicated candidate list. */
#include "central_scan.h"

#include <string.h>

#define AD_UUID16_INCOMPLETE 0x02u
#define AD_UUID16_COMPLETE 0x03u
#define AD_NAME_SHORT 0x08u
#define AD_NAME_COMPLETE 0x09u
#define AD_APPEARANCE 0x19u

static void copy_name(char out[CENTRAL_SCAN_NAME_BYTES], const uint8_t *src, size_t len)
{
    if (len > CENTRAL_SCAN_NAME_BYTES - 1) len = CENTRAL_SCAN_NAME_BYTES - 1;
    for (size_t i = 0; i < len; ++i) out[i] = (char)(src[i] < 0x20u || src[i] == 0x7Fu ? '?' : src[i]);
    out[len] = '\0';
}

void central_adv_parse(const uint8_t *data, size_t len, central_adv_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data) return;
    size_t pos = 0;
    bool complete_name = false;
    while (pos < len) {
        size_t field = data[pos];
        if (field == 0) break;                /* early terminator */
        if (field > len - pos - 1) break;     /* runs past the buffer */
        uint8_t type = data[pos + 1];
        const uint8_t *value = &data[pos + 2];
        size_t value_len = field - 1;
        switch (type) {
        case AD_UUID16_INCOMPLETE:
        case AD_UUID16_COMPLETE:
            if (value_len % 2 != 0) break;
            for (size_t i = 0; i + 1 < value_len; i += 2)
                if ((uint16_t)(value[i] | (value[i + 1] << 8)) == 0x1812u) out->hid_service = true;
            break;
        case AD_APPEARANCE:
            if (value_len == 2) {
                out->has_appearance = true;
                out->appearance = (uint16_t)(value[0] | (value[1] << 8));
            }
            break;
        case AD_NAME_SHORT:
        case AD_NAME_COMPLETE:
            if (complete_name && type == AD_NAME_SHORT) break; /* keep the complete name */
            copy_name(out->name, value, value_len);
            complete_name = type == AD_NAME_COMPLETE;
            break;
        default: break;
        }
        pos += field + 1;
    }
}

void central_scan_clear(central_scan_list_t *list) { memset(list, 0, sizeof(*list)); }

static uint8_t kind_of(const central_adv_info_t *info)
{
    if (!info->has_appearance) return 0;
    if (info->appearance == CENTRAL_APPEARANCE_KEYBOARD) return DOUBTECH_HID_CENTRAL_KEYBOARD;
    if (info->appearance == CENTRAL_APPEARANCE_MOUSE) return DOUBTECH_HID_CENTRAL_MOUSE;
    return 0;
}

int central_scan_offer(central_scan_list_t *list, const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                       const central_adv_info_t *info)
{
    if (!list || !addr || !info) return -1;
    uint8_t kind = kind_of(info);
    for (uint8_t i = 0; i < list->count; ++i) {
        central_scan_entry_t *entry = &list->entries[i];
        if (entry->addr_type != addr_type || memcmp(entry->addr, addr, 6) != 0) continue;
        entry->rssi = rssi;
        bool changed = false;
        if (info->name[0] && entry->name[0] == '\0') {
            memcpy(entry->name, info->name, sizeof(entry->name));
            changed = true;
        }
        if (kind && entry->kind == 0) {
            entry->kind = kind;
            changed = true;
        }
        return changed ? (int)i : -1;
    }
    if (!info->hid_service && kind == 0) return -1;
    if (list->count >= CENTRAL_SCAN_MAX) return -1;
    central_scan_entry_t *entry = &list->entries[list->count];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->addr, addr, 6);
    entry->addr_type = addr_type;
    entry->kind = kind;
    entry->rssi = rssi;
    memcpy(entry->name, info->name, sizeof(entry->name));
    entry->name[sizeof(entry->name) - 1] = '\0';
    return (int)list->count++;
}
