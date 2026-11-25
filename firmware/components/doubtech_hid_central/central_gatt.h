/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Pure HID-over-GATT discovery state machine for one central link. The BLE
 * adapter feeds it discovery results and completions; every step returns the
 * single next action (discover, read, write, ready or fail) for the adapter to
 * perform. It holds no platform types, so every path is host-testable.
 *
 * Path: service 0x1812 -> characteristics -> descriptors (CCCD 0x2902 and
 * Report Reference 0x2908 owners) -> then
 *   keyboard: boot input 0x2A22 required (else reason 1); Protocol Mode 0x2A4E
 *             written 0 when present; boot input subscribed.
 *   mouse:    with a Report Map 0x2A4B the map is read and parsed; a map that
 *             gives a wheel (or a device without boot mouse input) selects the
 *             Report 0x2A4D input whose Report Reference ID matches the map
 *             (Protocol Mode written 1); otherwise boot mouse 0x2A33 is used
 *             (Protocol Mode written 0). Neither usable -> reason 1. */
#ifndef DTHID_CENTRAL_GATT_H
#define DTHID_CENTRAL_GATT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doubtech_hid_central.h"
#include "hogp_parse.h"

#define CENTRAL_GATT_MAX_CHRS 20u
/** Report notifications longer than this are truncated before parsing. */
#define CENTRAL_GATT_MAX_REPORT_BYTES 64u

#define CENTRAL_UUID_HID_SERVICE 0x1812u
#define CENTRAL_UUID_PROTOCOL_MODE 0x2A4Eu
#define CENTRAL_UUID_BOOT_KEYBOARD_INPUT 0x2A22u
#define CENTRAL_UUID_BOOT_MOUSE_INPUT 0x2A33u
#define CENTRAL_UUID_REPORT_MAP 0x2A4Bu
#define CENTRAL_UUID_REPORT 0x2A4Du
#define CENTRAL_UUID_CCCD 0x2902u
#define CENTRAL_UUID_REPORT_REFERENCE 0x2908u

/* GATT characteristic property bits (Core spec Vol 3 Part G 3.3.1.1). */
#define CENTRAL_PROP_WRITE_NO_RSP 0x04u
#define CENTRAL_PROP_WRITE 0x08u
#define CENTRAL_PROP_NOTIFY 0x10u

typedef enum {
    CG_IDLE,
    CG_SERVICE,
    CG_CHARACTERISTICS,
    CG_DESCRIPTORS,
    CG_REPORT_MAP,
    CG_REPORT_REFERENCE,
    CG_PROTOCOL_MODE,
    CG_SUBSCRIBE,
    CG_READY,
    CG_FAILED
} central_gatt_state_t;

typedef enum {
    CG_ACT_NONE,
    CG_ACT_DISCOVER_SERVICE,         /* uuid16 = 0x1812 */
    CG_ACT_DISCOVER_CHARACTERISTICS, /* start..end */
    CG_ACT_DISCOVER_DESCRIPTORS,     /* start..end */
    CG_ACT_READ_LONG,                /* handle (Report Map) */
    CG_ACT_READ,                     /* handle (Report Reference) */
    CG_ACT_WRITE,                    /* handle, value[0..value_len), with response */
    CG_ACT_WRITE_NO_RSP,             /* handle, value; call central_gatt_written once queued */
    CG_ACT_READY,                    /* notifications enabled; input flows */
    CG_ACT_FAIL                      /* reason; terminate the link */
} central_gatt_action_type_t;

typedef struct {
    central_gatt_action_type_t type;
    uint16_t uuid16;
    uint16_t handle, start, end;
    uint8_t value[2];
    uint8_t value_len;
    uint8_t reason;
} central_gatt_action_t;

/** Error classes the adapter maps NimBLE statuses to. */
typedef enum { CG_ERR_OTHER, CG_ERR_TIMEOUT, CG_ERR_AUTH, CG_ERR_BUSY } central_gatt_error_t;

typedef enum { CG_INPUT_NONE, CG_INPUT_BOOT_KEYBOARD, CG_INPUT_BOOT_MOUSE, CG_INPUT_REPORT_MOUSE } central_gatt_input_t;

typedef struct {
    uint16_t uuid16; /* 0 for a 128-bit UUID */
    uint16_t def_handle, val_handle, end_handle;
    uint8_t props;
    uint16_t cccd, report_ref;
} central_gatt_chr_t;

typedef struct {
    central_gatt_state_t state;
    uint8_t kind_hint, allowed_kinds, kind;
    bool service_found;
    uint16_t svc_start, svc_end;
    central_gatt_chr_t chrs[CENTRAL_GATT_MAX_CHRS];
    uint8_t chr_count;
    bool chrs_full; /* later characteristics only close the previous range */
    int8_t protocol_mode, boot_keyboard, boot_mouse, report_map;
    uint8_t ref_cursor; /* next chrs index to try as the mouse Report */
    central_gatt_input_t mode;
    int8_t input;
    hogp_mouse_map_t map;
    bool map_ok;
} central_gatt_t;

/** Bit for allowed_kinds. */
#define CENTRAL_KIND_BIT(kind) ((uint8_t)(1u << (kind)))

/** Starts discovery. kind_hint is 0 (unknown, resolved from the characteristics:
 * boot keyboard input means keyboard, otherwise mouse) or a kind. A resolved kind
 * outside allowed_kinds fails with reason 5 (capacity). */
void central_gatt_begin(central_gatt_t *m, uint8_t kind_hint, uint8_t allowed_kinds, central_gatt_action_t *act);
/** One discovered HID service; only the first counts. */
void central_gatt_service(central_gatt_t *m, uint16_t start, uint16_t end);
void central_gatt_services_done(central_gatt_t *m, central_gatt_action_t *act);
/** One characteristic, in ascending handle order. */
void central_gatt_characteristic(central_gatt_t *m, uint16_t uuid16, uint16_t def_handle, uint16_t val_handle,
                                 uint8_t props);
void central_gatt_characteristics_done(central_gatt_t *m, central_gatt_action_t *act);
/** One attribute from the descriptor sweep over the service range. */
void central_gatt_descriptor(central_gatt_t *m, uint16_t uuid16, uint16_t handle);
void central_gatt_descriptors_done(central_gatt_t *m, central_gatt_action_t *act);
/** The complete (or 512-byte capped) Report Map value. */
void central_gatt_report_map(central_gatt_t *m, const uint8_t *data, size_t len, central_gatt_action_t *act);
/** The Report Map is longer than HOGP_MAP_MAX_BYTES: it is treated as an
 * unsupported map (boot mouse fallback, else reason 1), never parsed from a
 * truncated prefix. */
void central_gatt_report_map_too_long(central_gatt_t *m, central_gatt_action_t *act);
/** The value of the Report Reference descriptor last requested. */
void central_gatt_report_reference(central_gatt_t *m, const uint8_t *data, size_t len, central_gatt_action_t *act);
/** A Protocol Mode or CCCD write completed (or a write without response was queued). */
void central_gatt_written(central_gatt_t *m, central_gatt_action_t *act);
/** A procedure failed or could not start: fails with the class's reason
 * (other 1, timeout 2, auth 3, busy 4). Ignored outside discovery. */
void central_gatt_error(central_gatt_t *m, central_gatt_error_t error, central_gatt_action_t *act);

/** Converts one notification into input. False when the link is not ready, the
 * handle is not the subscribed input, or the payload does not parse. */
bool central_gatt_notify(const central_gatt_t *m, uint16_t handle, const uint8_t *data, size_t len,
                         doubtech_hid_central_input_t *out);
/** Resets the machine for a closed link. Returns true and fills a lost input
 * when the link was ready (input may be held downstream). */
bool central_gatt_disconnect(central_gatt_t *m, doubtech_hid_central_input_t *out);

/* ---- Stored-device reconnect schedule ------------------------------------
 * Pure cadence for reconnecting the stored keyboard and mouse, one global
 * schedule for both: one connection attempt of CENTRAL_RECONNECT_ATTEMPT_MS
 * (the ble_gap_connect duration) at a time, and after any attempt that ends
 * without a link (timed out, failed or refused) one shared gap of
 * CENTRAL_RECONNECT_GAP_MS plus 0 to CENTRAL_RECONNECT_JITTER_MS before the
 * next attempt of either kind, so scheduled attempts keep the initiator busy at most 10 s in every
 * 25 s and advertising and guest notifications keep their airtime. Attempts
 * alternate between the stored kinds (keyboard first), so with both absent
 * each is tried at least every 2 x (10 + 17) s, with one stored every 27 s.
 * None runs while an input pairing window is open, none to an identity that
 * is a guest's, and none beyond the central links the connection budget
 * leaves after the guest reserve. A ready link that drops (a keyboard gone to
 * sleep) is retried at once, at most once per CENTRAL_RECONNECT_FAST_MS per
 * kind; otherwise it follows the global schedule. The adapter owns the timer
 * and the random source; times are wrapping milliseconds. */

#define CENTRAL_RECONNECT_ATTEMPT_MS 10000u
#define CENTRAL_RECONNECT_GAP_MS 15000u
/** Added to each gap (0 to this many ms, from the caller's random value) so
 * the board's attempts do not stay in step with a device's advertising. */
#define CENTRAL_RECONNECT_JITTER_MS 2000u
/** A kind gets at most one immediate retry after a drop in this period. */
#define CENTRAL_RECONNECT_FAST_MS 30000u

typedef enum {
    CENTRAL_RECONNECT_NONE,    /* nothing to reconnect: stop the timer */
    CENTRAL_RECONNECT_WAIT,    /* evaluate again after delay_ms (or on the next link event) */
    CENTRAL_RECONNECT_CONNECT  /* start one attempt for kind now */
} central_reconnect_step_t;

typedef struct {
    central_reconnect_step_t step;
    uint8_t kind;
    uint32_t delay_ms;
} central_reconnect_action_t;

/** Global schedule state (kinds index DOUBTECH_HID_CENTRAL_KEYBOARD / _MOUSE). */
typedef struct {
    uint32_t next_ms;       /* earliest next scheduled attempt, any kind */
    uint8_t last_kind;      /* kind of the last attempt (round robin), 0 none */
    uint8_t urgent_kind;    /* a ready link of this kind dropped: one immediate attempt */
    bool fast_used[3];      /* an immediate retry was granted ... */
    uint32_t fast_at_ms[3]; /* ... at this time */
} central_reconnect_t;

/** What the adapter knows right now, indexed by kind. */
typedef struct {
    bool stored[3];      /* a record exists for the kind */
    bool guest[3];       /* the stored identity is a guest's: never connect to it */
    bool linked[3];      /* a link of the kind exists (connecting, encrypting, ready or closing) */
    bool pairing;        /* an input pairing window (scan or pairing link) is open */
    bool connecting;     /* an outgoing connection attempt is pending */
    uint8_t links;       /* central links in use, any phase */
    uint8_t link_budget; /* central links the connection budget allows */
} central_reconnect_facts_t;

/** The first attempt is due at now_ms (the boot attempt after the central starts). */
void central_reconnect_init(central_reconnect_t *r, uint32_t now_ms);
/** The next step for the given facts. */
central_reconnect_action_t central_reconnect_next(const central_reconnect_t *r, uint32_t now_ms,
                                                  const central_reconnect_facts_t *facts);
/** A CONNECT was started for kind (it is the round robin's last kind now). */
void central_reconnect_attempt_started(central_reconnect_t *r, uint8_t kind, uint32_t now_ms);
/** An attempt ended without a ready link (timed out, failed before ready, or
 * could not start, e.g. ENOMEM or EBUSY): every kind waits one gap plus a
 * jitter taken from random. */
void central_reconnect_attempt_failed(central_reconnect_t *r, uint8_t kind, uint32_t now_ms,
                                      uint32_t random);
/** A ready link of that kind dropped: one immediate attempt when none was
 * granted to that kind in the last CENTRAL_RECONNECT_FAST_MS; otherwise the
 * kind waits for the global schedule (at least one gap from now). */
void central_reconnect_link_lost(central_reconnect_t *r, uint8_t kind, uint32_t now_ms, uint32_t random);
/** The gap after a failed attempt for that random value: GAP + 0..JITTER ms. */
uint32_t central_reconnect_gap_ms(uint32_t random);
/** Central links left for input devices: max_connections less the guests'
 * reserve, capped at the central's own link count (0 when none are left). */
uint8_t central_reconnect_link_budget(unsigned max_connections, unsigned guest_reserve, unsigned central_links);

#endif
