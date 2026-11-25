/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares pure parsers for HID-over-GATT input reports: the fixed boot
 * keyboard and boot mouse layouts, and a bounded walker that derives a relative
 * mouse report layout from a HID report map. No platform state; every function
 * validates lengths and never reads past the supplied buffer. */
#ifndef HOGP_PARSE_H
#define HOGP_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The walker examines at most this many descriptor bytes. */
#define HOGP_MAP_MAX_BYTES 512u
/** Depth limit of the descriptor global push/pop stack. */
#define HOGP_MAP_MAX_GLOBAL_STACK 16u

/** Where the relative mouse fields live inside one input report. Bit offsets
 * count from the first bit of the notification payload, which never includes the
 * report ID byte (see hogp_parse_mouse_report). Buttons
 * always start at bit 0; only the low 8 buttons are reported. */
typedef struct {
    uint8_t report_id;
    bool has_id;
    uint8_t button_bits, x_bits, y_bits;
    uint8_t x_off, y_off, wheel_off, wheel_bits;
    bool has_wheel;
} hogp_mouse_map_t;

typedef enum { HOGP_MAP_OK, HOGP_MAP_UNSUPPORTED, HOGP_MAP_MALFORMED } hogp_map_result_t;

/** Copies an 8-byte boot keyboard report. Returns false unless len is exactly 8. */
bool hogp_parse_boot_keyboard(const uint8_t *data, size_t len, uint8_t keys[8]);

/** Reads a boot mouse report (buttons, signed 8-bit dx, dy). Needs len >= 3. */
bool hogp_parse_boot_mouse(const uint8_t *data, size_t len, uint8_t *buttons, int16_t *dx, int16_t *dy);

/** Derives the relative mouse layout from a report map. OK fills out; UNSUPPORTED
 * means a valid map that this walker will not interpret (absolute axes, no mouse,
 * long items, unsigned or oversized axes); MALFORMED means truncated or
 * inconsistent. out is untouched unless OK. */
hogp_map_result_t hogp_mouse_map_from_descriptor(const uint8_t *map, size_t len, hogp_mouse_map_t *out);

/** Extracts buttons (low 8), signed axes and wheel from one notification payload.
 * Contract: data is the bare payload of a Report characteristic notification and
 * carries NO report ID byte. The ID is known from the characteristic's Report
 * Reference descriptor (0x2908); the caller selects the characteristic whose
 * reference ID equals map->report_id (when map->has_id). The boot keyboard and
 * boot mouse parsers above also take the bare payload. Offsets in map are relative
 * to the payload start. Returns false on a short payload or an invalid map; a
 * payload wrongly prefixed with an ID byte cannot be detected and would shift
 * every field. wheel may be NULL. */
bool hogp_parse_mouse_report(const hogp_mouse_map_t *map, const uint8_t *data, size_t len, uint8_t *buttons,
                             int16_t *dx, int16_t *dy, int8_t *wheel);

#endif
