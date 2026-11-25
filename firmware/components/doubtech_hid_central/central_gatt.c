/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Pure HID-over-GATT discovery state machine (see central_gatt.h). Every entry
 * point validates the current state, so a late or duplicated completion after
 * a failure or a reset yields CG_ACT_NONE instead of a stray procedure. */
#include "central_gatt.h"

#include <string.h>

#define PROTOCOL_BOOT 0u
#define PROTOCOL_REPORT 1u
#define REPORT_TYPE_INPUT 1u

static void set_none(central_gatt_action_t *act) { memset(act, 0, sizeof(*act)); }

static void fail(central_gatt_t *m, uint8_t reason, central_gatt_action_t *act)
{
    set_none(act);
    m->state = CG_FAILED;
    m->mode = CG_INPUT_NONE;
    m->input = -1;
    act->type = CG_ACT_FAIL;
    act->reason = reason;
}

static bool notifies(const central_gatt_chr_t *chr) { return (chr->props & CENTRAL_PROP_NOTIFY) != 0; }

void central_gatt_begin(central_gatt_t *m, uint8_t kind_hint, uint8_t allowed_kinds, central_gatt_action_t *act)
{
    memset(m, 0, sizeof(*m));
    m->kind_hint = kind_hint;
    m->allowed_kinds = allowed_kinds;
    m->protocol_mode = m->boot_keyboard = m->boot_mouse = m->report_map = -1;
    m->input = -1;
    m->state = CG_SERVICE;
    set_none(act);
    act->type = CG_ACT_DISCOVER_SERVICE;
    act->uuid16 = CENTRAL_UUID_HID_SERVICE;
}

void central_gatt_service(central_gatt_t *m, uint16_t start, uint16_t end)
{
    if (m->state != CG_SERVICE || m->service_found || start == 0 || end < start) return;
    m->service_found = true;
    m->svc_start = start;
    m->svc_end = end;
}

void central_gatt_services_done(central_gatt_t *m, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_SERVICE) return;
    if (!m->service_found) {
        fail(m, DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED, act);
        return;
    }
    m->state = CG_CHARACTERISTICS;
    act->type = CG_ACT_DISCOVER_CHARACTERISTICS;
    act->start = m->svc_start;
    act->end = m->svc_end;
}

void central_gatt_characteristic(central_gatt_t *m, uint16_t uuid16, uint16_t def_handle, uint16_t val_handle,
                                 uint8_t props)
{
    if (m->state != CG_CHARACTERISTICS || val_handle <= def_handle || def_handle < m->svc_start ||
        val_handle > m->svc_end)
        return;
    if (m->chr_count) {
        central_gatt_chr_t *previous = &m->chrs[m->chr_count - 1];
        if (def_handle <= previous->val_handle) return; /* out of order */
        if (previous->end_handle == 0) previous->end_handle = (uint16_t)(def_handle - 1);
    }
    if (m->chr_count >= CENTRAL_GATT_MAX_CHRS) {
        m->chrs_full = true;
        return;
    }
    central_gatt_chr_t *chr = &m->chrs[m->chr_count++];
    memset(chr, 0, sizeof(*chr));
    chr->uuid16 = uuid16;
    chr->def_handle = def_handle;
    chr->val_handle = val_handle;
    chr->props = props;
}

static int8_t find_chr(const central_gatt_t *m, uint16_t uuid16)
{
    for (uint8_t i = 0; i < m->chr_count; ++i)
        if (m->chrs[i].uuid16 == uuid16) return (int8_t)i;
    return -1;
}

void central_gatt_characteristics_done(central_gatt_t *m, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_CHARACTERISTICS) return;
    for (uint8_t i = 0; i < m->chr_count; ++i)
        if (m->chrs[i].end_handle == 0) m->chrs[i].end_handle = m->svc_end;
    m->protocol_mode = find_chr(m, CENTRAL_UUID_PROTOCOL_MODE);
    m->boot_keyboard = find_chr(m, CENTRAL_UUID_BOOT_KEYBOARD_INPUT);
    m->boot_mouse = find_chr(m, CENTRAL_UUID_BOOT_MOUSE_INPUT);
    m->report_map = find_chr(m, CENTRAL_UUID_REPORT_MAP);
    if (m->kind_hint == DOUBTECH_HID_CENTRAL_KEYBOARD || m->kind_hint == DOUBTECH_HID_CENTRAL_MOUSE)
        m->kind = m->kind_hint;
    else
        m->kind = m->boot_keyboard >= 0 ? DOUBTECH_HID_CENTRAL_KEYBOARD : DOUBTECH_HID_CENTRAL_MOUSE;
    if (!(m->allowed_kinds & CENTRAL_KIND_BIT(m->kind))) {
        fail(m, DOUBTECH_HID_CENTRAL_REASON_CAPACITY, act);
        return;
    }
    bool usable = m->kind == DOUBTECH_HID_CENTRAL_KEYBOARD
                      ? m->boot_keyboard >= 0 && notifies(&m->chrs[m->boot_keyboard])
                      : (m->boot_mouse >= 0 && notifies(&m->chrs[m->boot_mouse])) || m->report_map >= 0;
    if (!usable) {
        fail(m, DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED, act);
        return;
    }
    m->state = CG_DESCRIPTORS;
    act->type = CG_ACT_DISCOVER_DESCRIPTORS;
    act->start = m->svc_start;
    act->end = m->svc_end;
}

void central_gatt_descriptor(central_gatt_t *m, uint16_t uuid16, uint16_t handle)
{
    if (m->state != CG_DESCRIPTORS) return;
    if (uuid16 != CENTRAL_UUID_CCCD && uuid16 != CENTRAL_UUID_REPORT_REFERENCE) return;
    for (uint8_t i = 0; i < m->chr_count; ++i) {
        central_gatt_chr_t *chr = &m->chrs[i];
        if (handle <= chr->val_handle || handle > chr->end_handle) continue;
        uint16_t *slot = uuid16 == CENTRAL_UUID_CCCD ? &chr->cccd : &chr->report_ref;
        if (*slot == 0) *slot = handle;
        return;
    }
}

/* Next step after the input characteristic is chosen: the Protocol Mode write
 * when the device has a writable one, otherwise the CCCD write. */
static void protocol_or_subscribe(central_gatt_t *m, uint8_t protocol, central_gatt_action_t *act)
{
    set_none(act);
    if (m->protocol_mode >= 0) {
        const central_gatt_chr_t *pm = &m->chrs[m->protocol_mode];
        if (pm->props & (CENTRAL_PROP_WRITE_NO_RSP | CENTRAL_PROP_WRITE)) {
            m->state = CG_PROTOCOL_MODE;
            /* HIDS declares Protocol Mode as write-without-response; prefer it. */
            act->type = (pm->props & CENTRAL_PROP_WRITE_NO_RSP) ? CG_ACT_WRITE_NO_RSP : CG_ACT_WRITE;
            act->handle = pm->val_handle;
            act->value[0] = protocol;
            act->value_len = 1;
            return;
        }
    }
    m->state = CG_SUBSCRIBE;
    act->type = CG_ACT_WRITE;
    act->handle = m->chrs[m->input].cccd;
    act->value[0] = 0x01; /* notifications */
    act->value[1] = 0x00;
    act->value_len = 2;
}

static bool subscribable(const central_gatt_t *m, int8_t index)
{
    return index >= 0 && notifies(&m->chrs[index]) && m->chrs[index].cccd != 0;
}

static void use_boot_mouse_or_fail(central_gatt_t *m, central_gatt_action_t *act)
{
    if (!subscribable(m, m->boot_mouse)) {
        fail(m, DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED, act);
        return;
    }
    m->mode = CG_INPUT_BOOT_MOUSE;
    m->input = m->boot_mouse;
    protocol_or_subscribe(m, PROTOCOL_BOOT, act);
}

void central_gatt_descriptors_done(central_gatt_t *m, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_DESCRIPTORS) return;
    if (m->kind == DOUBTECH_HID_CENTRAL_KEYBOARD) {
        if (!subscribable(m, m->boot_keyboard)) {
            fail(m, DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED, act);
            return;
        }
        m->mode = CG_INPUT_BOOT_KEYBOARD;
        m->input = m->boot_keyboard;
        protocol_or_subscribe(m, PROTOCOL_BOOT, act);
        return;
    }
    if (m->report_map >= 0) {
        m->state = CG_REPORT_MAP;
        act->type = CG_ACT_READ_LONG;
        act->handle = m->chrs[m->report_map].val_handle;
        return;
    }
    use_boot_mouse_or_fail(m, act);
}

/* Requests the next Report characteristic that could carry the mouse input
 * (notifies, has a CCCD and a Report Reference); false when none is left. */
static bool next_reference(central_gatt_t *m, central_gatt_action_t *act)
{
    for (; m->ref_cursor < m->chr_count; ++m->ref_cursor) {
        const central_gatt_chr_t *chr = &m->chrs[m->ref_cursor];
        if (chr->uuid16 != CENTRAL_UUID_REPORT || !notifies(chr) || !chr->cccd || !chr->report_ref) continue;
        set_none(act);
        m->state = CG_REPORT_REFERENCE;
        act->type = CG_ACT_READ;
        act->handle = chr->report_ref;
        return true;
    }
    return false;
}

void central_gatt_report_map(central_gatt_t *m, const uint8_t *data, size_t len, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_REPORT_MAP) return;
    hogp_mouse_map_t map;
    memset(&map, 0, sizeof(map));
    m->map_ok = data && hogp_mouse_map_from_descriptor(data, len, &map) == HOGP_MAP_OK;
    if (m->map_ok) m->map = map;
    bool boot_available = subscribable(m, m->boot_mouse);
    /* Boot first; the report map is used for a wheel or when boot input is missing. */
    if (m->map_ok && (m->map.has_wheel || !boot_available)) {
        m->ref_cursor = 0;
        if (next_reference(m, act)) return;
    }
    use_boot_mouse_or_fail(m, act);
}

void central_gatt_report_map_too_long(central_gatt_t *m, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_REPORT_MAP) return;
    m->map_ok = false;
    use_boot_mouse_or_fail(m, act);
}

void central_gatt_report_reference(central_gatt_t *m, const uint8_t *data, size_t len, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state != CG_REPORT_REFERENCE) return;
    bool match = data && len >= 2 && data[1] == REPORT_TYPE_INPUT && (!m->map.has_id || data[0] == m->map.report_id);
    if (match) {
        m->mode = CG_INPUT_REPORT_MOUSE;
        m->input = (int8_t)m->ref_cursor;
        protocol_or_subscribe(m, PROTOCOL_REPORT, act);
        return;
    }
    m->ref_cursor++;
    if (next_reference(m, act)) return;
    use_boot_mouse_or_fail(m, act);
}

void central_gatt_written(central_gatt_t *m, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state == CG_PROTOCOL_MODE) {
        m->protocol_mode = -1; /* written; the next step is the CCCD */
        protocol_or_subscribe(m, 0, act);
        return;
    }
    if (m->state == CG_SUBSCRIBE) {
        m->state = CG_READY;
        act->type = CG_ACT_READY;
    }
}

void central_gatt_error(central_gatt_t *m, central_gatt_error_t error, central_gatt_action_t *act)
{
    set_none(act);
    if (m->state == CG_IDLE || m->state == CG_READY || m->state == CG_FAILED) return;
    uint8_t reason = error == CG_ERR_TIMEOUT ? DOUBTECH_HID_CENTRAL_REASON_TIMEOUT
                     : error == CG_ERR_AUTH  ? DOUBTECH_HID_CENTRAL_REASON_AUTH_FAILED
                     : error == CG_ERR_BUSY  ? DOUBTECH_HID_CENTRAL_REASON_BUSY
                                             : DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED;
    fail(m, reason, act);
}

bool central_gatt_notify(const central_gatt_t *m, uint16_t handle, const uint8_t *data, size_t len,
                         doubtech_hid_central_input_t *out)
{
    if (!m || !out || !data || m->state != CG_READY || m->input < 0 || m->chrs[m->input].val_handle != handle)
        return false;
    doubtech_hid_central_input_t input;
    memset(&input, 0, sizeof(input));
    input.kind = m->kind;
    bool ok = false;
    switch (m->mode) {
    case CG_INPUT_BOOT_KEYBOARD: ok = hogp_parse_boot_keyboard(data, len, input.keys); break;
    case CG_INPUT_BOOT_MOUSE: ok = hogp_parse_boot_mouse(data, len, &input.buttons, &input.dx, &input.dy); break;
    case CG_INPUT_REPORT_MOUSE:
        ok = hogp_parse_mouse_report(&m->map, data, len, &input.buttons, &input.dx, &input.dy, &input.wheel);
        break;
    default: break;
    }
    if (ok) *out = input;
    return ok;
}

bool central_gatt_disconnect(central_gatt_t *m, doubtech_hid_central_input_t *out)
{
    bool was_ready = m->state == CG_READY;
    uint8_t kind = m->kind;
    memset(m, 0, sizeof(*m));
    m->protocol_mode = m->boot_keyboard = m->boot_mouse = m->report_map = -1;
    m->input = -1;
    if (!was_ready || !out) return false;
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->lost = true;
    return true;
}
