/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the boot report parsers and a bounded HID report-map walker. The
 * walker understands short items only, tracks the global push/pop stack, and
 * recognises just the Generic Desktop X, Y and Wheel usages and the Button page
 * inside the first mouse application collection. Anything it cannot place
 * exactly is rejected with a reason instead of being scaled by guesswork. */
#include "hogp_parse.h"

#include <string.h>

#define MAX_USAGES 16u
#define MAX_REPORT_IDS 16u
#define MAX_FIELD_COUNT 256u
#define MAX_BIT_OFFSET 0xFFFFFFu

#define PAGE_GENERIC_DESKTOP 0x01u
#define PAGE_BUTTON 0x09u
#define USAGE_MOUSE 0x02u
#define USAGE_X 0x30u
#define USAGE_Y 0x31u
#define USAGE_WHEEL 0x38u

bool hogp_parse_boot_keyboard(const uint8_t *data, size_t len, uint8_t keys[8]) {
    if (data == NULL || keys == NULL || len != 8u) { return false; }
    memcpy(keys, data, 8u);
    return true;
}

bool hogp_parse_boot_mouse(const uint8_t *data, size_t len, uint8_t *buttons, int16_t *dx, int16_t *dy) {
    if (data == NULL || buttons == NULL || dx == NULL || dy == NULL || len < 3u) { return false; }
    *buttons = data[0];
    *dx = (int8_t)data[1];
    *dy = (int8_t)data[2];
    return true;
}

typedef struct {
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t size, count;
    uint8_t id;
} globals_t;

typedef struct {
    uint16_t page; /* 0 means "the usage page current at the main item" */
    uint16_t usage;
} usage_t;

typedef struct {
    usage_t list[MAX_USAGES];
    size_t n;
    bool overflow;
    bool has_range;
    uint16_t range_page, range_min, range_max;
} locals_t;

typedef struct {
    uint8_t id;
    uint32_t pos;
} id_pos_t;

static uint32_t item_unsigned(const uint8_t *p, size_t size) {
    uint32_t v = 0;
    for (size_t i = 0; i < size; i++) { v |= (uint32_t)p[i] << (8u * i); }
    return v;
}

static int32_t item_signed(const uint8_t *p, size_t size) {
    uint32_t v = item_unsigned(p, size);
    if (size == 0u) { return 0; }
    if (size < 4u && (v & (1u << (8u * size - 1u))) != 0u) { v |= ~0u << (8u * size); }
    return (int32_t)v;
}

static id_pos_t *id_slot(id_pos_t *table, size_t *used, uint8_t id) {
    for (size_t i = 0; i < *used; i++) {
        if (table[i].id == id) { return &table[i]; }
    }
    if (*used >= MAX_REPORT_IDS) { return NULL; }
    table[*used].id = id;
    table[*used].pos = 0;
    return &table[(*used)++];
}

static void resolve_usage(const locals_t *l, const globals_t *g, uint32_t k, uint16_t *page, uint16_t *usage) {
    if (l->n > 0u) {
        const usage_t *u = &l->list[k < l->n ? k : l->n - 1u];
        *page = u->page != 0u ? u->page : g->page;
        *usage = u->usage;
    } else if (l->has_range) {
        uint32_t u = (uint32_t)l->range_min + k;
        *page = l->range_page != 0u ? l->range_page : g->page;
        *usage = (uint16_t)(u > l->range_max ? l->range_max : u);
    } else {
        *page = 0;
        *usage = 0;
    }
}

typedef struct {
    uint32_t btn_start, btn_count;
    bool has_x, has_y, has_wheel;
    uint32_t x_off, y_off, wheel_off;
    uint32_t x_bits, y_bits, wheel_bits;
    bool id_set;
    uint8_t id;
} mouse_acc_t;

/* Records one mouse-collection variable field; returns OK or a rejection. */
static hogp_map_result_t mouse_field(mouse_acc_t *m, const globals_t *g, uint32_t flags, uint16_t page, uint16_t usage,
                                     uint32_t off) {
    if (page == PAGE_BUTTON) {
        if (g->size != 1u) { return HOGP_MAP_UNSUPPORTED; }
        if (m->btn_count == 0u) {
            m->btn_start = off;
        } else if (off != m->btn_start + m->btn_count) {
            return HOGP_MAP_UNSUPPORTED;
        }
        m->btn_count++;
        return HOGP_MAP_OK;
    }
    if (page != PAGE_GENERIC_DESKTOP || (usage != USAGE_X && usage != USAGE_Y && usage != USAGE_WHEEL)) {
        return HOGP_MAP_OK;
    }
    if ((flags & 0x04u) == 0u) { return HOGP_MAP_UNSUPPORTED; } /* absolute */
    uint32_t max_bits = usage == USAGE_WHEEL ? 8u : 16u;
    if (g->size == 0u || g->size > max_bits || off > 255u) { return HOGP_MAP_UNSUPPORTED; }
    if (g->lmin >= 0) { return HOGP_MAP_UNSUPPORTED; } /* unsigned axis: layout cannot say so */
    int64_t lo = -((int64_t)1 << (g->size - 1u));
    int64_t hi = ((int64_t)1 << (g->size - 1u)) - 1;
    if ((int64_t)g->lmin < lo || (int64_t)g->lmax > hi || g->lmax < g->lmin) { return HOGP_MAP_MALFORMED; }
    bool *seen = usage == USAGE_X ? &m->has_x : usage == USAGE_Y ? &m->has_y : &m->has_wheel;
    uint32_t *o = usage == USAGE_X ? &m->x_off : usage == USAGE_Y ? &m->y_off : &m->wheel_off;
    uint32_t *b = usage == USAGE_X ? &m->x_bits : usage == USAGE_Y ? &m->y_bits : &m->wheel_bits;
    if (*seen) { return HOGP_MAP_UNSUPPORTED; }
    *seen = true;
    *o = off;
    *b = g->size;
    return HOGP_MAP_OK;
}

hogp_map_result_t hogp_mouse_map_from_descriptor(const uint8_t *map, size_t len, hogp_mouse_map_t *out) {
    if (map == NULL || out == NULL || len == 0u) { return HOGP_MAP_MALFORMED; }
    const size_t limit = len > HOGP_MAP_MAX_BYTES ? HOGP_MAP_MAX_BYTES : len;
    const hogp_map_result_t cut = len > HOGP_MAP_MAX_BYTES ? HOGP_MAP_UNSUPPORTED : HOGP_MAP_MALFORMED;
    globals_t g;
    memset(&g, 0, sizeof g);
    globals_t stack[HOGP_MAP_MAX_GLOBAL_STACK];
    size_t sp = 0;
    locals_t l;
    memset(&l, 0, sizeof l);
    id_pos_t ids[MAX_REPORT_IDS];
    size_t id_used = 0;
    mouse_acc_t m;
    memset(&m, 0, sizeof m);
    uint32_t depth = 0;
    bool in_mouse = false, mouse_seen = false;
    size_t i = 0;

    while (i < limit) {
        const uint8_t prefix = map[i++];
        if (prefix == 0xFEu) { return HOGP_MAP_UNSUPPORTED; } /* long item */
        size_t size = prefix & 3u;
        if (size == 3u) { size = 4u; }
        const unsigned type = (prefix >> 2) & 3u;
        const unsigned tag = prefix >> 4;
        if (limit - i < size) { return cut; }
        const uint8_t *data = map + i;
        i += size;
        const uint32_t uval = item_unsigned(data, size);

        if (type == 1u) { /* global */
            switch (tag) {
            case 0x0: g.page = (uint16_t)uval; break;
            case 0x1: g.lmin = item_signed(data, size); break;
            case 0x2: g.lmax = item_signed(data, size); break;
            case 0x7: g.size = uval; break;
            case 0x8:
                if (uval == 0u || uval > 255u) { return HOGP_MAP_MALFORMED; }
                g.id = (uint8_t)uval;
                break;
            case 0x9: g.count = uval; break;
            case 0xA:
                if (sp >= HOGP_MAP_MAX_GLOBAL_STACK) { return HOGP_MAP_MALFORMED; }
                stack[sp++] = g;
                break;
            case 0xB:
                if (sp == 0u) { return HOGP_MAP_MALFORMED; }
                g = stack[--sp];
                break;
            default: break;
            }
        } else if (type == 2u) { /* local */
            uint16_t page = size == 4u ? (uint16_t)(uval >> 16) : 0u;
            switch (tag) {
            case 0x0:
                if (l.n < MAX_USAGES) {
                    l.list[l.n].page = page;
                    l.list[l.n].usage = (uint16_t)uval;
                    l.n++;
                } else {
                    l.overflow = true;
                }
                break;
            case 0x1:
                l.has_range = true;
                l.range_page = page;
                l.range_min = (uint16_t)uval;
                if (l.range_max < l.range_min) { l.range_max = l.range_min; }
                break;
            case 0x2:
                l.range_max = (uint16_t)uval;
                break;
            default: break;
            }
        } else if (type == 0u) { /* main */
            if (tag == 0xAu) { /* collection */
                if (depth == 0u && !mouse_seen && uval == 1u && l.n > 0u) {
                    uint16_t page, usage;
                    resolve_usage(&l, &g, 0, &page, &usage);
                    if (page == PAGE_GENERIC_DESKTOP && usage == USAGE_MOUSE) { in_mouse = mouse_seen = true; }
                }
                depth++;
            } else if (tag == 0xCu) { /* end collection */
                if (depth == 0u) { return HOGP_MAP_MALFORMED; }
                depth--;
                if (depth == 0u && in_mouse) { break; }
            } else if (tag == 0x8u) { /* input */
                id_pos_t *slot = id_slot(ids, &id_used, g.id);
                if (slot == NULL) { return HOGP_MAP_UNSUPPORTED; }
                const uint32_t start = slot->pos;
                if (in_mouse && (uval & 1u) == 0u && (uval & 2u) != 0u) {
                    if (m.id_set && m.id != g.id) { return HOGP_MAP_UNSUPPORTED; }
                    m.id_set = true;
                    m.id = g.id;
                    if (g.count > MAX_FIELD_COUNT || l.overflow) { return HOGP_MAP_UNSUPPORTED; }
                    for (uint32_t k = 0; k < g.count; k++) {
                        uint16_t page, usage;
                        resolve_usage(&l, &g, k, &page, &usage);
                        uint64_t off = (uint64_t)start + (uint64_t)k * g.size;
                        if (off > MAX_BIT_OFFSET) { return HOGP_MAP_UNSUPPORTED; }
                        hogp_map_result_t r = mouse_field(&m, &g, uval, page, usage, (uint32_t)off);
                        if (r != HOGP_MAP_OK) { return r; }
                    }
                }
                uint64_t end = (uint64_t)start + (uint64_t)g.size * g.count;
                slot->pos = end > MAX_BIT_OFFSET ? MAX_BIT_OFFSET : (uint32_t)end;
            }
            memset(&l, 0, sizeof l);
        }
    }

    if (depth != 0u) { return cut; }
    if (!mouse_seen || !m.has_x || !m.has_y) { return HOGP_MAP_UNSUPPORTED; }
    if (m.btn_count > 0u && m.btn_start != 0u) { return HOGP_MAP_UNSUPPORTED; }
    if (m.btn_count > 255u) { return HOGP_MAP_UNSUPPORTED; }
    out->has_id = m.id != 0u;
    out->report_id = m.id;
    out->button_bits = (uint8_t)m.btn_count;
    out->x_bits = (uint8_t)m.x_bits;
    out->y_bits = (uint8_t)m.y_bits;
    out->x_off = (uint8_t)m.x_off;
    out->y_off = (uint8_t)m.y_off;
    out->has_wheel = m.has_wheel;
    out->wheel_off = m.has_wheel ? (uint8_t)m.wheel_off : 0u;
    out->wheel_bits = m.has_wheel ? (uint8_t)m.wheel_bits : 0u;
    return HOGP_MAP_OK;
}

/* LSB-first extraction across byte boundaries; caller guarantees bounds. */
static uint32_t get_bits(const uint8_t *p, uint32_t bit_off, uint32_t bits) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bits; i++) {
        uint32_t b = bit_off + i;
        v |= (uint32_t)((p[b >> 3] >> (b & 7u)) & 1u) << i;
    }
    return v;
}

static int32_t sign_extend(uint32_t v, uint32_t bits) {
    if ((v & (1u << (bits - 1u))) != 0u) { return (int32_t)v - (int32_t)(1u << bits); }
    return (int32_t)v;
}

bool hogp_parse_mouse_report(const hogp_mouse_map_t *map, const uint8_t *data, size_t len, uint8_t *buttons,
                             int16_t *dx, int16_t *dy, int8_t *wheel) {
    if (map == NULL || data == NULL || buttons == NULL || dx == NULL || dy == NULL) { return false; }
    if (map->x_bits == 0u || map->x_bits > 16u || map->y_bits == 0u || map->y_bits > 16u) { return false; }
    if (map->has_wheel && (map->wheel_bits == 0u || map->wheel_bits > 8u)) { return false; }
    uint32_t need_bits = map->button_bits;
    if ((uint32_t)map->x_off + map->x_bits > need_bits) { need_bits = (uint32_t)map->x_off + map->x_bits; }
    if ((uint32_t)map->y_off + map->y_bits > need_bits) { need_bits = (uint32_t)map->y_off + map->y_bits; }
    if (map->has_wheel && (uint32_t)map->wheel_off + map->wheel_bits > need_bits) {
        need_bits = (uint32_t)map->wheel_off + map->wheel_bits;
    }
    if (len < ((size_t)need_bits + 7u) / 8u) { return false; }
    *buttons = (uint8_t)get_bits(data, 0u, map->button_bits < 8u ? map->button_bits : 8u);
    *dx = (int16_t)sign_extend(get_bits(data, map->x_off, map->x_bits), map->x_bits);
    *dy = (int16_t)sign_extend(get_bits(data, map->y_off, map->y_bits), map->y_bits);
    if (wheel != NULL) {
        *wheel = map->has_wheel ? (int8_t)sign_extend(get_bits(data, map->wheel_off, map->wheel_bits), map->wheel_bits)
                                : 0;
    }
    return true;
}
