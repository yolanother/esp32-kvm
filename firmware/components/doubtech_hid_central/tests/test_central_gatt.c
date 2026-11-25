/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Drives the pure HOGP discovery state machine through simulated GATT
 * databases: boot keyboard, boot mouse, a wheel mouse through its report map
 * (with and without a report ID), unsupported devices (reason 1), capacity
 * (reason 5), an injected error in every discovery state, and the lost input a
 * ready link reports when it closes. Also drives the stored-device reconnect
 * schedule: 10 s attempts 15 s apart, one at a time, none during a pairing
 * scan, never to a guest identity, within the connection budget. */
#include <stdio.h>
#include <string.h>

#include "central_gatt.h"

static int failures;
#define CHECK(c)                                               \
    do {                                                       \
        if (!(c)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            failures++;                                        \
        }                                                      \
    } while (0)

#define KB DOUBTECH_HID_CENTRAL_KEYBOARD
#define MOUSE DOUBTECH_HID_CENTRAL_MOUSE
#define BOTH (CENTRAL_KIND_BIT(KB) | CENTRAL_KIND_BIT(MOUSE))
#define RD 0x02u
#define RN (0x02u | CENTRAL_PROP_NOTIFY)
#define RWN (0x02u | CENTRAL_PROP_WRITE_NO_RSP)
#define RW (0x02u | CENTRAL_PROP_WRITE)

typedef struct { uint16_t uuid, def, val; uint8_t props; } fx_chr;
typedef struct { uint16_t uuid, handle; } fx_dsc;
typedef struct { uint16_t handle; uint8_t value[2]; } fx_ref;
typedef struct {
    bool has_service;
    uint16_t start, end;
    const fx_chr *chrs;
    size_t chr_count;
    const fx_dsc *dscs;
    size_t dsc_count;
    const uint8_t *map;
    size_t map_len;
    const fx_ref *refs;
    size_t ref_count;
    central_gatt_state_t fail_in; /* CG_IDLE: no injected error */
    central_gatt_error_t fail_class;
    bool map_too_long; /* the device's Report Map exceeds the 512-byte cap */
} fixture_t;

typedef struct {
    unsigned writes, reads, long_reads;
    uint16_t write_handle[4];
    uint8_t write_value[4][2];
    uint8_t write_len[4];
    bool write_no_rsp[4];
} trace_t;

/* Wheel mouse with report ID 2: buttons(3)+pad, X, Y, wheel 8-bit relative. */
static const uint8_t k_wheel_id2[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
/* Same without a report ID. */
static const uint8_t k_wheel[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
/* Boot-format mouse map: no wheel. */
static const uint8_t k_no_wheel[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
/* Absolute X/Y (tablet): the walker reports UNSUPPORTED. */
static const uint8_t k_absolute[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0, 0xC0};

/* Keyboard: Protocol Mode (write without response), Report Map, one Report
 * input, Boot Keyboard Input with CCCD, Boot Keyboard Output, HID Information. */
static const fx_chr k_kb_chrs[] = {
    {0x2A4E, 0x11, 0x12, RWN}, {0x2A4B, 0x13, 0x14, RD}, {0x2A4D, 0x15, 0x16, RN},
    {0x2A22, 0x19, 0x1A, RN},  {0x2A32, 0x1C, 0x1D, 0x0E}, {0x2A4A, 0x1E, 0x1F, RD}};
static const fx_dsc k_kb_dscs[] = {{0x2803, 0x11}, {0x2A4E, 0x12}, {0x2803, 0x13}, {0x2A4B, 0x14},
                                   {0x2803, 0x15}, {0x2A4D, 0x16}, {0x2902, 0x17}, {0x2908, 0x18},
                                   {0x2803, 0x19}, {0x2A22, 0x1A}, {0x2902, 0x1B}, {0x2803, 0x1C},
                                   {0x2A32, 0x1D}, {0x2803, 0x1E}, {0x2A4A, 0x1F}};

/* Mouse: Protocol Mode (write), Report Map, Report id 1 (output-type), Report id 2
 * input, Boot Mouse Input. */
static const fx_chr k_mouse_chrs[] = {{0x2A4E, 0x21, 0x22, RW},
                                      {0x2A4B, 0x23, 0x24, RD},
                                      {0x2A4D, 0x25, 0x26, RN},
                                      {0x2A4D, 0x29, 0x2A, RN},
                                      {0x2A33, 0x2D, 0x2E, RN}};
static const fx_dsc k_mouse_dscs[] = {{0x2902, 0x27}, {0x2908, 0x28}, {0x2902, 0x2B},
                                      {0x2908, 0x2C}, {0x2902, 0x2F}};
static const fx_ref k_mouse_refs_id2[] = {{0x28, {0x01, 0x02}}, {0x2C, {0x02, 0x01}}};
static const fx_ref k_mouse_refs_id0[] = {{0x28, {0x00, 0x02}}, {0x2C, {0x00, 0x01}}};
static const fx_ref k_mouse_refs_none[] = {{0x28, {0x05, 0x01}}, {0x2C, {0x06, 0x01}}};

/* Mouse with neither boot input nor a usable map: only a Report characteristic. */
static const fx_chr k_report_only_chrs[] = {{0x2A4D, 0x41, 0x42, RN}};
static const fx_dsc k_report_only_dscs[] = {{0x2902, 0x43}, {0x2908, 0x44}};

static fixture_t keyboard_fixture(void)
{
    fixture_t f = {true, 0x10, 0x1F, k_kb_chrs, 6, k_kb_dscs, 15, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    return f;
}

static fixture_t mouse_fixture(const uint8_t *map, size_t map_len, const fx_ref *refs)
{
    fixture_t f = {true, 0x20, 0x2F, k_mouse_chrs, 5, k_mouse_dscs, 5, map, map_len, refs, 2, CG_IDLE, CG_ERR_OTHER};
    return f;
}

static central_gatt_action_t drive(central_gatt_t *m, const fixture_t *f, uint8_t hint, uint8_t allowed, trace_t *t)
{
    central_gatt_action_t act;
    memset(t, 0, sizeof(*t));
    central_gatt_begin(m, hint, allowed, &act);
    for (int steps = 0; steps < 64; ++steps) {
        bool pending = act.type != CG_ACT_NONE && act.type != CG_ACT_READY && act.type != CG_ACT_FAIL;
        if (pending && f->fail_in != CG_IDLE && m->state == f->fail_in) {
            central_gatt_error(m, f->fail_class, &act);
            continue;
        }
        switch (act.type) {
        case CG_ACT_DISCOVER_SERVICE:
            CHECK(act.uuid16 == CENTRAL_UUID_HID_SERVICE);
            if (f->has_service) central_gatt_service(m, f->start, f->end);
            central_gatt_services_done(m, &act);
            break;
        case CG_ACT_DISCOVER_CHARACTERISTICS:
            CHECK(act.start == f->start && act.end == f->end);
            for (size_t i = 0; i < f->chr_count; ++i)
                central_gatt_characteristic(m, f->chrs[i].uuid, f->chrs[i].def, f->chrs[i].val, f->chrs[i].props);
            central_gatt_characteristics_done(m, &act);
            break;
        case CG_ACT_DISCOVER_DESCRIPTORS:
            CHECK(act.start == f->start && act.end == f->end);
            for (size_t i = 0; i < f->dsc_count; ++i) central_gatt_descriptor(m, f->dscs[i].uuid, f->dscs[i].handle);
            central_gatt_descriptors_done(m, &act);
            break;
        case CG_ACT_READ_LONG:
            t->long_reads++;
            CHECK(m->report_map >= 0 && act.handle == m->chrs[m->report_map].val_handle);
            CHECK(act.handle == 0x24 || act.handle == 0x42);
            if (f->map_too_long) central_gatt_report_map_too_long(m, &act);
            else central_gatt_report_map(m, f->map, f->map_len, &act);
            break;
        case CG_ACT_READ: {
            const fx_ref *ref = NULL;
            t->reads++;
            for (size_t i = 0; i < f->ref_count; ++i)
                if (f->refs[i].handle == act.handle) ref = &f->refs[i];
            CHECK(ref != NULL);
            if (ref) central_gatt_report_reference(m, ref->value, 2, &act);
            else central_gatt_error(m, CG_ERR_OTHER, &act);
            break;
        }
        case CG_ACT_WRITE:
        case CG_ACT_WRITE_NO_RSP:
            if (t->writes < 4) {
                t->write_handle[t->writes] = act.handle;
                memcpy(t->write_value[t->writes], act.value, 2);
                t->write_len[t->writes] = act.value_len;
                t->write_no_rsp[t->writes] = act.type == CG_ACT_WRITE_NO_RSP;
            }
            t->writes++;
            central_gatt_written(m, &act);
            break;
        default:
            return act;
        }
    }
    CHECK(!"discovery did not terminate");
    return act;
}

static void test_boot_keyboard_happy_path(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = keyboard_fixture();
    central_gatt_action_t act = drive(&m, &f, KB, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.state == CG_READY);
    CHECK(m.kind == KB && m.mode == CG_INPUT_BOOT_KEYBOARD);
    CHECK(t.long_reads == 0 && t.reads == 0);
    /* Protocol Mode 0 (Boot) without response, then the boot input CCCD. */
    CHECK(t.writes == 2);
    CHECK(t.write_handle[0] == 0x12 && t.write_len[0] == 1 && t.write_value[0][0] == 0 && t.write_no_rsp[0]);
    CHECK(t.write_handle[1] == 0x1B && t.write_len[1] == 2 && t.write_value[1][0] == 1 && t.write_value[1][1] == 0);
    CHECK(!t.write_no_rsp[1]);

    const uint8_t report[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};
    doubtech_hid_central_input_t in;
    CHECK(central_gatt_notify(&m, 0x1A, report, 8, &in));
    CHECK(in.kind == KB && !in.lost && memcmp(in.keys, report, 8) == 0 && in.buttons == 0 && in.dx == 0);
    CHECK(!central_gatt_notify(&m, 0x16, report, 8, &in)); /* not the subscribed input */
    CHECK(!central_gatt_notify(&m, 0x1A, report, 7, &in)); /* boot keyboard is exactly 8 bytes */
}

static void test_keyboard_kind_resolved_without_hint(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = keyboard_fixture();
    central_gatt_action_t act = drive(&m, &f, 0, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.kind == KB);
}

static void test_protocol_mode_write_variants(void)
{
    /* Protocol Mode with the Write property uses a write request. */
    static const fx_chr chrs[] = {{0x2A4E, 0x11, 0x12, RW}, {0x2A22, 0x13, 0x14, RN}};
    static const fx_dsc dscs[] = {{0x2902, 0x15}};
    fixture_t f = {true, 0x10, 0x15, chrs, 2, dscs, 1, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    central_gatt_t m;
    trace_t t;
    CHECK(drive(&m, &f, KB, BOTH, &t).type == CG_ACT_READY);
    CHECK(t.writes == 2 && !t.write_no_rsp[0] && t.write_handle[0] == 0x12 && t.write_handle[1] == 0x15);

    /* Without Protocol Mode the CCCD is written directly. */
    static const fx_chr only_boot[] = {{0x2A22, 0x13, 0x14, RN}};
    fixture_t g = {true, 0x10, 0x15, only_boot, 1, dscs, 1, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    CHECK(drive(&m, &g, KB, BOTH, &t).type == CG_ACT_READY);
    CHECK(t.writes == 1 && t.write_handle[0] == 0x15);

    /* A read-only Protocol Mode is skipped. */
    static const fx_chr ro_pm[] = {{0x2A4E, 0x11, 0x12, RD}, {0x2A22, 0x13, 0x14, RN}};
    fixture_t h = {true, 0x10, 0x15, ro_pm, 2, dscs, 1, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    CHECK(drive(&m, &h, KB, BOTH, &t).type == CG_ACT_READY);
    CHECK(t.writes == 1 && t.write_handle[0] == 0x15);
}

static void test_boot_mouse(void)
{
    central_gatt_t m;
    trace_t t;
    /* A map without a wheel keeps the boot path. */
    fixture_t f = mouse_fixture(k_no_wheel, sizeof(k_no_wheel), k_mouse_refs_id2);
    central_gatt_action_t act = drive(&m, &f, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.kind == MOUSE && m.mode == CG_INPUT_BOOT_MOUSE);
    CHECK(t.long_reads == 1 && t.reads == 0);
    CHECK(t.writes == 2 && t.write_handle[0] == 0x22 && t.write_value[0][0] == 0 && !t.write_no_rsp[0]);
    CHECK(t.write_handle[1] == 0x2F && t.write_value[1][0] == 1);
    const uint8_t report[4] = {0x03, 0x7F, 0x80, 0x55};
    doubtech_hid_central_input_t in;
    CHECK(central_gatt_notify(&m, 0x2E, report, 3, &in));
    CHECK(in.kind == MOUSE && in.buttons == 3 && in.dx == 127 && in.dy == -128 && in.wheel == 0 && !in.lost);
    CHECK(!central_gatt_notify(&m, 0x2E, report, 2, &in));

    /* No report map at all: boot mouse, no map read, kind resolved as mouse. */
    static const fx_chr chrs[] = {{0x2A33, 0x2D, 0x2E, RN}};
    static const fx_dsc dscs[] = {{0x2902, 0x2F}};
    fixture_t g = {true, 0x20, 0x2F, chrs, 1, dscs, 1, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    act = drive(&m, &g, 0, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.kind == MOUSE && m.mode == CG_INPUT_BOOT_MOUSE && t.long_reads == 0);
    CHECK(t.writes == 1 && t.write_handle[0] == 0x2F);

    /* An unsupported (absolute) map falls back to boot mouse input. */
    fixture_t h = mouse_fixture(k_absolute, sizeof(k_absolute), k_mouse_refs_id2);
    act = drive(&m, &h, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.mode == CG_INPUT_BOOT_MOUSE);
}

static void test_mouse_report_map_accepted(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = mouse_fixture(k_wheel_id2, sizeof(k_wheel_id2), k_mouse_refs_id2);
    central_gatt_action_t act = drive(&m, &f, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.mode == CG_INPUT_REPORT_MOUSE && m.map_ok && m.map.has_id);
    CHECK(t.long_reads == 1 && t.reads == 2); /* first reference is id 1 output-type, second matches */
    /* Protocol Mode 1 (Report), then the matching report's CCCD. */
    CHECK(t.writes == 2 && t.write_handle[0] == 0x22 && t.write_value[0][0] == 1);
    CHECK(t.write_handle[1] == 0x2B && t.write_value[1][0] == 1);
    const uint8_t payload[4] = {0x01, 0x05, 0xFB, 0xFF}; /* bare payload, no ID byte */
    doubtech_hid_central_input_t in;
    CHECK(central_gatt_notify(&m, 0x2A, payload, 4, &in));
    CHECK(in.kind == MOUSE && in.buttons == 1 && in.dx == 5 && in.dy == -5 && in.wheel == -1);
    CHECK(!central_gatt_notify(&m, 0x2E, payload, 4, &in)); /* boot input not subscribed */
    CHECK(!central_gatt_notify(&m, 0x2A, payload, 3, &in));

    /* Map without a report ID takes the first input-type reference. */
    fixture_t g = mouse_fixture(k_wheel, sizeof(k_wheel), k_mouse_refs_id0);
    act = drive(&m, &g, 0, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.kind == MOUSE && m.mode == CG_INPUT_REPORT_MOUSE);
    CHECK(t.write_handle[1] == 0x2B);
    CHECK(central_gatt_notify(&m, 0x2A, payload, 4, &in) && in.wheel == -1);

    /* No reference matches the map's ID: fall back to boot input. */
    fixture_t h = mouse_fixture(k_wheel_id2, sizeof(k_wheel_id2), k_mouse_refs_none);
    act = drive(&m, &h, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.mode == CG_INPUT_BOOT_MOUSE);
    CHECK(t.write_handle[0] == 0x22 && t.write_value[0][0] == 0 && t.write_handle[1] == 0x2F);
}

static void test_unsupported_devices_fail_with_reason_1(void)
{
    central_gatt_t m;
    trace_t t;
    /* No HID service. */
    fixture_t none = {false, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    central_gatt_action_t act = drive(&m, &none, 0, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED && m.state == CG_FAILED);

    /* A keyboard without boot keyboard input (Report mode only). */
    fixture_t kb = {true, 0x40, 0x44, k_report_only_chrs, 1, k_report_only_dscs, 2, NULL, 0, NULL, 0, CG_IDLE,
                    CG_ERR_OTHER};
    act = drive(&m, &kb, KB, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED);
    CHECK(t.writes == 0);

    /* Unknown kind, a Report characteristic but no map and no boot input. */
    act = drive(&m, &kb, 0, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED);

    /* A mouse whose only map is unsupported and that has no boot input. */
    static const fx_chr chrs[] = {{0x2A4B, 0x41, 0x42, RD}, {0x2A4D, 0x43, 0x44, RN}};
    static const fx_dsc dscs[] = {{0x2902, 0x45}, {0x2908, 0x46}};
    static const fx_ref refs[] = {{0x46, {0x00, 0x01}}};
    fixture_t abs_only = {true, 0x40, 0x46, chrs, 2, dscs, 2, k_absolute, sizeof(k_absolute), refs, 1, CG_IDLE,
                          CG_ERR_OTHER};
    act = drive(&m, &abs_only, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED);
    CHECK(t.long_reads == 1 && t.writes == 0);

    /* A boot keyboard input with no CCCD cannot be subscribed. */
    static const fx_chr no_cccd[] = {{0x2A22, 0x11, 0x12, RN}};
    fixture_t nc = {true, 0x10, 0x12, no_cccd, 1, NULL, 0, NULL, 0, NULL, 0, CG_IDLE, CG_ERR_OTHER};
    act = drive(&m, &nc, KB, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED);
}

static void test_capacity_reason_5(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = keyboard_fixture();
    /* The device turns out to be a keyboard but only a mouse slot is free. */
    central_gatt_action_t act = drive(&m, &f, 0, CENTRAL_KIND_BIT(MOUSE), &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_CAPACITY && t.writes == 0);
    fixture_t g = mouse_fixture(k_wheel, sizeof(k_wheel), k_mouse_refs_id0);
    act = drive(&m, &g, MOUSE, CENTRAL_KIND_BIT(KB), &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_CAPACITY && t.long_reads == 0);
}

static void expect_error_in(fixture_t f, uint8_t hint, central_gatt_state_t state)
{
    static const struct { central_gatt_error_t error; uint8_t reason; } classes[] = {
        {CG_ERR_OTHER, DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED},
        {CG_ERR_TIMEOUT, DOUBTECH_HID_CENTRAL_REASON_TIMEOUT},
        {CG_ERR_AUTH, DOUBTECH_HID_CENTRAL_REASON_AUTH_FAILED},
        {CG_ERR_BUSY, DOUBTECH_HID_CENTRAL_REASON_BUSY}};
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        central_gatt_t m;
        trace_t t;
        f.fail_in = state;
        f.fail_class = classes[i].error;
        central_gatt_action_t act = drive(&m, &f, hint, BOTH, &t);
        if (act.type != CG_ACT_FAIL || act.reason != classes[i].reason || m.state != CG_FAILED)
            printf("  state %d class %d -> act %d reason %d\n", (int)state, (int)classes[i].error, (int)act.type,
                   (int)act.reason);
        CHECK(act.type == CG_ACT_FAIL && act.reason == classes[i].reason && m.state == CG_FAILED);
        /* A failed machine ignores late completions and never yields input. */
        central_gatt_written(&m, &act);
        CHECK(act.type == CG_ACT_NONE && m.state == CG_FAILED);
        central_gatt_error(&m, CG_ERR_OTHER, &act);
        CHECK(act.type == CG_ACT_NONE);
        doubtech_hid_central_input_t in;
        const uint8_t report[8] = {0};
        CHECK(!central_gatt_notify(&m, 0x1A, report, 8, &in));
        CHECK(!central_gatt_disconnect(&m, &in));
    }
}

static void test_discovery_error_at_each_state(void)
{
    fixture_t kb = keyboard_fixture();
    expect_error_in(kb, KB, CG_SERVICE);
    expect_error_in(kb, KB, CG_CHARACTERISTICS);
    expect_error_in(kb, KB, CG_DESCRIPTORS);
    expect_error_in(kb, KB, CG_PROTOCOL_MODE);
    expect_error_in(kb, KB, CG_SUBSCRIBE);
    fixture_t mouse = mouse_fixture(k_wheel_id2, sizeof(k_wheel_id2), k_mouse_refs_id2);
    expect_error_in(mouse, MOUSE, CG_REPORT_MAP);
    expect_error_in(mouse, MOUSE, CG_REPORT_REFERENCE);
    expect_error_in(mouse, MOUSE, CG_PROTOCOL_MODE);
    expect_error_in(mouse, MOUSE, CG_SUBSCRIBE);
}

static void test_out_of_order_events_ignored(void)
{
    central_gatt_t m;
    central_gatt_action_t act;
    central_gatt_begin(&m, KB, BOTH, &act);
    CHECK(act.type == CG_ACT_DISCOVER_SERVICE && m.state == CG_SERVICE);
    central_gatt_written(&m, &act);
    CHECK(act.type == CG_ACT_NONE && m.state == CG_SERVICE);
    central_gatt_descriptors_done(&m, &act);
    CHECK(act.type == CG_ACT_NONE && m.state == CG_SERVICE);
    const uint8_t ref[2] = {1, 1};
    central_gatt_report_reference(&m, ref, 2, &act);
    CHECK(act.type == CG_ACT_NONE && m.state == CG_SERVICE);
    /* A second service is ignored; the first range is used. */
    central_gatt_service(&m, 0x10, 0x1F);
    central_gatt_service(&m, 0x50, 0x5F);
    central_gatt_services_done(&m, &act);
    CHECK(act.type == CG_ACT_DISCOVER_CHARACTERISTICS && act.start == 0x10 && act.end == 0x1F);
}

static void test_characteristic_overflow_is_bounded(void)
{
    central_gatt_t m;
    central_gatt_action_t act;
    central_gatt_begin(&m, 0, BOTH, &act);
    central_gatt_service(&m, 0x0001, 0x00FF);
    central_gatt_services_done(&m, &act);
    for (uint16_t i = 0; i < CENTRAL_GATT_MAX_CHRS + 5; ++i)
        central_gatt_characteristic(&m, 0x2A4D, (uint16_t)(2 + 3 * i), (uint16_t)(3 + 3 * i), RN);
    CHECK(m.chr_count == CENTRAL_GATT_MAX_CHRS);
    /* The last stored characteristic ends just before the first dropped one. */
    CHECK(m.chrs[CENTRAL_GATT_MAX_CHRS - 1].end_handle == (uint16_t)(2 + 3 * CENTRAL_GATT_MAX_CHRS - 1));
    central_gatt_characteristics_done(&m, &act);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED);
}

static void test_lost_input_on_disconnect(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = keyboard_fixture();
    CHECK(drive(&m, &f, KB, BOTH, &t).type == CG_ACT_READY);
    doubtech_hid_central_input_t in;
    memset(&in, 0xAA, sizeof(in));
    CHECK(central_gatt_disconnect(&m, &in));
    CHECK(in.lost && in.kind == KB && in.buttons == 0 && in.dx == 0 && in.dy == 0 && in.wheel == 0);
    static const uint8_t zero[8] = {0};
    CHECK(memcmp(in.keys, zero, 8) == 0);
    CHECK(m.state == CG_IDLE);
    CHECK(!central_gatt_disconnect(&m, &in)); /* only once */
    const uint8_t report[8] = {0, 0, 4, 0, 0, 0, 0, 0};
    CHECK(!central_gatt_notify(&m, 0x1A, report, 8, &in));

    fixture_t g = mouse_fixture(k_wheel_id2, sizeof(k_wheel_id2), k_mouse_refs_id2);
    CHECK(drive(&m, &g, MOUSE, BOTH, &t).type == CG_ACT_READY);
    CHECK(central_gatt_disconnect(&m, &in) && in.lost && in.kind == MOUSE);

    /* A link that closes during discovery delivered no input: nothing to release. */
    central_gatt_action_t act;
    central_gatt_begin(&m, KB, BOTH, &act);
    CHECK(!central_gatt_disconnect(&m, &in) && m.state == CG_IDLE);
}

/* A map over the 512-byte cap is never parsed from a truncated prefix: it counts
 * as unsupported, so a boot mouse falls back to boot input and anything else
 * fails with reason 1. */
static void test_report_map_too_long(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = mouse_fixture(k_wheel_id2, sizeof(k_wheel_id2), k_mouse_refs_id2);
    f.map_too_long = true;
    central_gatt_action_t act = drive(&m, &f, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_READY && m.mode == CG_INPUT_BOOT_MOUSE && !m.map_ok && t.reads == 0);

    static const fx_chr chrs[] = {{0x2A4B, 0x41, 0x42, RD}, {0x2A4D, 0x43, 0x44, RN}};
    static const fx_dsc dscs[] = {{0x2902, 0x45}, {0x2908, 0x46}};
    static const fx_ref refs[] = {{0x46, {0x02, 0x01}}};
    fixture_t g = {true,      0x40, 0x46,         chrs,         2, dscs, 2, k_wheel_id2, sizeof(k_wheel_id2), refs, 1,
                   CG_IDLE, CG_ERR_OTHER, true};
    act = drive(&m, &g, MOUSE, BOTH, &t);
    CHECK(act.type == CG_ACT_FAIL && act.reason == DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED && t.reads == 0);

    /* Outside the map read it is ignored. */
    central_gatt_begin(&m, MOUSE, BOTH, &act);
    central_gatt_report_map_too_long(&m, &act);
    CHECK(act.type == CG_ACT_NONE && m.state == CG_SERVICE);
}

/* The adapter resets the machine when it aborts a ready link (forget, failure)
 * and again when the link finally closes: the lost input comes exactly once,
 * from whichever reset sees READY first. */
static void test_abort_from_ready_reports_lost_once(void)
{
    central_gatt_t m;
    trace_t t;
    fixture_t f = keyboard_fixture();
    CHECK(drive(&m, &f, KB, BOTH, &t).type == CG_ACT_READY);
    doubtech_hid_central_input_t in;
    int lost = 0;
    if (central_gatt_disconnect(&m, &in) && in.lost && in.kind == KB) lost++; /* abort */
    if (central_gatt_disconnect(&m, &in)) lost++;                             /* later DISCONNECT */
    CHECK(lost == 1);

    /* A plain disconnect also reports once. */
    CHECK(drive(&m, &f, KB, BOTH, &t).type == CG_ACT_READY);
    lost = 0;
    if (central_gatt_disconnect(&m, &in) && in.lost) lost++;
    if (central_gatt_disconnect(&m, &in)) lost++;
    CHECK(lost == 1);

    /* A machine discarded without an output (failure before input flowed) reports nothing later. */
    CHECK(drive(&m, &f, KB, BOTH, &t).type == CG_ACT_READY);
    CHECK(!central_gatt_disconnect(&m, NULL));
    CHECK(!central_gatt_disconnect(&m, &in));
}

/* ---- stored-device reconnect schedule ---- */

static central_reconnect_facts_t facts_both_stored(void)
{
    central_reconnect_facts_t f;
    memset(&f, 0, sizeof f);
    f.stored[KB] = f.stored[MOUSE] = true;
    f.link_budget = 2;
    return f;
}

/* Starts the attempt the schedule asked for: the link now exists and is connecting. */
static void start_attempt(central_reconnect_facts_t *f, uint8_t kind)
{
    f->linked[kind] = true;
    f->connecting = true;
    f->links++;
}

static void end_attempt(central_reconnect_facts_t *f, uint8_t kind)
{
    f->linked[kind] = false;
    f->connecting = false;
    f->links--;
}

static void test_reconnect_boot_one_attempt_at_a_time(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    central_reconnect_init(&r, 1000);
    /* Boot: the keyboard goes first, at once. */
    central_reconnect_action_t a = central_reconnect_next(&r, 1000, &f);
    CHECK(a.step == CENTRAL_RECONNECT_CONNECT && a.kind == KB);
    start_attempt(&f, KB);
    /* The mouse is due too, but one outgoing attempt at a time. */
    a = central_reconnect_next(&r, 1000, &f);
    CHECK(a.step == CENTRAL_RECONNECT_WAIT && a.delay_ms > 0);
    CHECK(central_reconnect_next(&r, 1000 + CENTRAL_RECONNECT_ATTEMPT_MS - 1, &f).step != CENTRAL_RECONNECT_CONNECT);
    /* The 10 s attempt times out: the mouse takes its turn, the keyboard waits 15 s. */
    uint32_t t = 1000 + CENTRAL_RECONNECT_ATTEMPT_MS;
    end_attempt(&f, KB);
    central_reconnect_attempt_failed(&r, KB, t);
    a = central_reconnect_next(&r, t, &f);
    CHECK(a.step == CENTRAL_RECONNECT_CONNECT && a.kind == MOUSE);
}
_Static_assert(CENTRAL_RECONNECT_ATTEMPT_MS == 10000u && CENTRAL_RECONNECT_GAP_MS == 15000u,
               "spec: 10 s attempts, 15 s apart");

static void test_reconnect_retries_after_gap(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    f.stored[MOUSE] = false;
    central_reconnect_init(&r, 0);
    uint32_t t = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        central_reconnect_action_t a = central_reconnect_next(&r, t, &f);
        CHECK(a.step == CENTRAL_RECONNECT_CONNECT && a.kind == KB);
        start_attempt(&f, KB);
        t += CENTRAL_RECONNECT_ATTEMPT_MS;
        end_attempt(&f, KB);
        central_reconnect_attempt_failed(&r, KB, t);
        a = central_reconnect_next(&r, t, &f);
        CHECK(a.step == CENTRAL_RECONNECT_WAIT && a.delay_ms == CENTRAL_RECONNECT_GAP_MS);
        CHECK(central_reconnect_next(&r, t + CENTRAL_RECONNECT_GAP_MS - 1, &f).step == CENTRAL_RECONNECT_WAIT);
        t += CENTRAL_RECONNECT_GAP_MS;
    }
    /* A connect the controller refused (ENOMEM, EBUSY) is a normal retry too. */
    central_reconnect_attempt_failed(&r, KB, t);
    CHECK(central_reconnect_next(&r, t, &f).step == CENTRAL_RECONNECT_WAIT);
    CHECK(central_reconnect_next(&r, t + CENTRAL_RECONNECT_GAP_MS, &f).step == CENTRAL_RECONNECT_CONNECT);
}

static void test_reconnect_stops_when_forgotten_or_connected(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    central_reconnect_init(&r, 0);
    f.stored[KB] = f.stored[MOUSE] = false; /* forgotten */
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_NONE);
    CHECK(central_reconnect_next(&r, 999999u, &f).step == CENTRAL_RECONNECT_NONE);
    /* Both connected: nothing to do either. */
    f = facts_both_stored();
    f.linked[KB] = f.linked[MOUSE] = true;
    f.links = 2;
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_NONE);
    /* Only the mouse forgotten: only the keyboard is ever attempted. */
    f = facts_both_stored();
    f.stored[MOUSE] = false;
    for (uint32_t t = 0; t < 120000u; t += 5000u) {
        central_reconnect_action_t a = central_reconnect_next(&r, t, &f);
        CHECK(a.step != CENTRAL_RECONNECT_CONNECT || a.kind == KB);
    }
}

static void test_reconnect_resumes_after_disconnect(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    f.stored[MOUSE] = false;
    central_reconnect_init(&r, 0);
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_CONNECT);
    start_attempt(&f, KB);
    f.connecting = false; /* connected and ready */
    CHECK(central_reconnect_next(&r, 50000u, &f).step == CENTRAL_RECONNECT_NONE);
    /* The keyboard went to sleep: the first attempt is immediate, then the usual cadence. */
    end_attempt(&f, KB);
    central_reconnect_link_lost(&r, KB, 60000u);
    central_reconnect_action_t a = central_reconnect_next(&r, 60000u, &f);
    CHECK(a.step == CENTRAL_RECONNECT_CONNECT && a.kind == KB);
    start_attempt(&f, KB);
    end_attempt(&f, KB);
    central_reconnect_attempt_failed(&r, KB, 70000u);
    a = central_reconnect_next(&r, 70000u, &f);
    CHECK(a.step == CENTRAL_RECONNECT_WAIT && a.delay_ms == CENTRAL_RECONNECT_GAP_MS);
    CHECK(central_reconnect_next(&r, 85000u, &f).step == CENTRAL_RECONNECT_CONNECT);
}

static void test_reconnect_never_during_pairing_scan(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    central_reconnect_init(&r, 0);
    f.pairing = true;
    for (uint32_t t = 0; t <= 2u * DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS; t += 1000u) {
        central_reconnect_action_t a = central_reconnect_next(&r, t, &f);
        CHECK(a.step == CENTRAL_RECONNECT_WAIT && a.delay_ms > 0);
    }
    /* The window ended: the overdue keyboard goes at once. */
    f.pairing = false;
    central_reconnect_action_t a = central_reconnect_next(&r, 61000u, &f);
    CHECK(a.step == CENTRAL_RECONNECT_CONNECT && a.kind == KB);
}

static void test_reconnect_never_to_a_guest_identity(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    central_reconnect_init(&r, 0);
    f.guest[KB] = true; /* a stale record that now names a guest's identity */
    for (uint32_t t = 0; t < 200000u; t += 2500u) {
        central_reconnect_action_t a = central_reconnect_next(&r, t, &f);
        CHECK(a.step != CENTRAL_RECONNECT_CONNECT || a.kind == MOUSE);
    }
    f.guest[MOUSE] = true;
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_NONE);
}

static void test_reconnect_respects_the_connection_budget(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    central_reconnect_init(&r, 0);
    /* The guest reserve leaves no central link (for example a smaller budget). */
    f.link_budget = 0;
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_WAIT);
    /* One link allowed and in use by the (connected) mouse: the keyboard waits. */
    f.link_budget = 1;
    f.linked[MOUSE] = true;
    f.links = 1;
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_WAIT);
    f.link_budget = 2;
    CHECK(central_reconnect_next(&r, 0, &f).step == CENTRAL_RECONNECT_CONNECT);
    /* The budget from the configured connection count, less the guest reserve. */
    CHECK(central_reconnect_link_budget(5, 3, 2) == 2);
    CHECK(central_reconnect_link_budget(4, 3, 2) == 1);
    CHECK(central_reconnect_link_budget(3, 3, 2) == 0);
    CHECK(central_reconnect_link_budget(2, 3, 2) == 0);
    CHECK(central_reconnect_link_budget(9, 3, 2) == 2);
}

static void test_reconnect_clock_wraps(void)
{
    central_reconnect_t r;
    central_reconnect_facts_t f = facts_both_stored();
    f.stored[MOUSE] = false;
    uint32_t t = 0xFFFFF000u;
    central_reconnect_init(&r, t);
    CHECK(central_reconnect_next(&r, t, &f).step == CENTRAL_RECONNECT_CONNECT);
    central_reconnect_attempt_failed(&r, KB, t);
    central_reconnect_action_t a = central_reconnect_next(&r, t + 1000u, &f);
    CHECK(a.step == CENTRAL_RECONNECT_WAIT && a.delay_ms == CENTRAL_RECONNECT_GAP_MS - 1000u);
    CHECK(central_reconnect_next(&r, t + CENTRAL_RECONNECT_GAP_MS, &f).step == CENTRAL_RECONNECT_CONNECT);
}

int main(void)
{
    test_reconnect_boot_one_attempt_at_a_time();
    test_reconnect_retries_after_gap();
    test_reconnect_stops_when_forgotten_or_connected();
    test_reconnect_resumes_after_disconnect();
    test_reconnect_never_during_pairing_scan();
    test_reconnect_never_to_a_guest_identity();
    test_reconnect_respects_the_connection_budget();
    test_reconnect_clock_wraps();
    test_report_map_too_long();
    test_abort_from_ready_reports_lost_once();
    test_boot_keyboard_happy_path();
    test_keyboard_kind_resolved_without_hint();
    test_protocol_mode_write_variants();
    test_boot_mouse();
    test_mouse_report_map_accepted();
    test_unsupported_devices_fail_with_reason_1();
    test_capacity_reason_5();
    test_discovery_error_at_each_state();
    test_out_of_order_events_ignored();
    test_characteristic_overflow_is_bounded();
    test_lost_input_on_disconnect();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
