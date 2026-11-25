/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * NimBLE adapter of the HID-over-GATT central. It shares the host that
 * doubtech_hid initializes and never touches nimble_port or ble_hs_cfg
 * callbacks; scanning and connecting use their own per-procedure GAP callback,
 * so every event of a central link reaches only this file.
 *
 * All state below is owned by the NimBLE host thread. Public calls from other
 * tasks run on the host loop through one bounded request. Link callbacks carry
 * (slot, generation) so a GATT or GAP callback for a closed or reused slot is
 * ignored. A link is released by its DISCONNECT, a failed CONNECT, or a close
 * that cannot complete (terminate refused, or the pairing close guard expires);
 * link_closed is the single place that reports PAIR_FAILED and DISCONNECTED.
 * The lost input is reported once, by whichever comes first: link_abort (a
 * forget or failure of a ready link) or link_closed. Discovery errors terminate
 * the link; an unfinished pairing deletes the bond it created.
 *
 * Stored devices reconnect on the pure schedule in central_gatt.h: from start
 * (the host is synced by then), after every closed stored link and after a
 * pairing window or a forget, one evaluation runs on the host loop and either
 * starts one attempt, arms the reconnect timer, or stops it. A stored link
 * that cannot finish its connect or close within a guard time is dropped so a
 * wedged procedure never blocks the schedule; later events miss its generation. */
#include "doubtech_hid_central.h"

#include <string.h>

#include "central_ble.h"
#include "central_gatt.h"
#include "central_registry.h"
#include "central_scan.h"
#include "central_store.h"
#include "doubtech_hid.h"
#include "esp_log.h"
#include "esp_random.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"

_Static_assert(DOUBTECH_HID_CENTRAL_KEYBOARD == DTHID_INPUT_KEYBOARD, "kind values must match the registry");
_Static_assert(DOUBTECH_HID_CENTRAL_MOUSE == DTHID_INPUT_MOUSE, "kind values must match the registry");
_Static_assert(CENTRAL_SCAN_NAME_BYTES == DTHID_INPUT_NAME_BYTES, "scan and record names share a size");
_Static_assert(DOUBTECH_HID_CENTRAL_NAME_BYTES == DTHID_INPUT_NAME_BYTES, "public and record names share a size");

#define LINK_COUNT 2u                /* one keyboard and one mouse */
#define CONNECT_TIMEOUT_MS ((int32_t)CENTRAL_RECONNECT_ATTEMPT_MS) /* one connection attempt */
#define PAIR_COMPLETE_MS 30000u      /* select to PAIRED, passkey typing included */
#define RPC_TIMEOUT_MS 200u
#define SCAN_ITVL 0x0060u            /* 60 ms */
#define SCAN_WINDOW 0x0030u          /* 30 ms: half duty keeps guest airtime */
#define CLOSE_GUARD_MS 5000u         /* a link that does not close by then is dropped */
#define REASON(name) DOUBTECH_HID_CENTRAL_REASON_##name

static const char *tag = "dthid-central";

typedef enum { LINK_FREE, LINK_CONNECTING, LINK_SECURING, LINK_DISCOVERING, LINK_READY, LINK_CLOSING } link_phase_t;

typedef struct {
    link_phase_t phase;
    uint16_t gen;
    uint16_t conn_handle;
    bool pairing;       /* belongs to the open pairing window and is not yet stored */
    bool silent;        /* closed by the owner: report no PAIR_FAILED */
    bool bonded;        /* encrypted with a bond on this link */
    bool announced;     /* CONNECTED was reported */
    uint8_t fail_reason;
    uint8_t kind_hint, allowed, kind; /* kind: resolved by discovery (0 until then) */
    ble_npl_time_t guard_at; /* stored link: drop it if still connecting or closing then */
    ble_addr_t addr;
    ble_addr_t peer_id; /* identity address once encrypted (bond owner) */
    char name[DTHID_INPUT_NAME_BYTES];
    central_gatt_t gatt;
    uint8_t map[HOGP_MAP_MAX_BYTES];
    uint16_t map_len;
} link_t;

typedef enum { PAIR_IDLE, PAIR_SCANNING, PAIR_LINKING } pair_phase_t;

static struct {
    pair_phase_t phase;
    bool saved_mitm;
    bool scan_deferred; /* waiting for a cancelled stored connect to release the master */
    int8_t link;
    central_scan_list_t list;
} pair;

typedef enum { OP_PAIR_BEGIN, OP_PAIR_CANCEL, OP_PAIR_SELECT, OP_FORGET } rpc_op_t;

static struct {
    struct ble_npl_event event;
    struct ble_npl_mutex lock;
    struct ble_npl_sem done;
    bool busy, finished, abandoned;
    rpc_op_t op;
    uint8_t arg;
    esp_err_t result;
    ble_npl_time_t deadline;
} rpc;

static bool started;
static doubtech_hid_central_config_t config;
static char store_ns[DTHID_INPUTS_NAMESPACE_MAX_CHARS + 1];
static central_registry_t registry;
static uint8_t own_addr_type;
static volatile bool kind_connected[3];
static link_t links[LINK_COUNT];
static uint16_t next_gen = 1;
static struct ble_npl_callout pair_timer;
/* Reconnect schedule (host thread) and its timer, immediate kick and link guard. */
static central_reconnect_t reconnect;
static struct ble_npl_callout reconnect_timer;
static struct ble_npl_event reconnect_event;
static struct ble_npl_callout guard_timer;
/* Stored devices as other tasks may read them (doubtech_hid_central_stored). */
static struct ble_npl_mutex stored_lock;
static struct {
    bool stored;
    char name[DTHID_INPUT_NAME_BYTES];
} published[3];

static const struct ble_gap_conn_params conn_params = {
    .scan_itvl = SCAN_ITVL,
    .scan_window = SCAN_WINDOW,
    .itvl_min = 12,  /* 15 ms */
    .itvl_max = 24,  /* 30 ms */
    .latency = 0,
    .supervision_timeout = 256, /* 2.56 s */
    .min_ce_len = 0,
    .max_ce_len = 0,
};

static int gap_event(struct ble_gap_event *event, void *arg);
static void run(link_t *link, central_gatt_action_t act);

/* ---- events -------------------------------------------------------------- */

static void emit(doubtech_hid_central_event_type_t type, uint8_t index, uint8_t kind, const char *name, int8_t rssi,
                 uint32_t passkey, uint8_t reason)
{
    if (!config.on_event) return;
    doubtech_hid_central_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.index = index;
    event.kind = kind;
    if (name) {
        memcpy(event.name, name, sizeof(event.name));
        event.name[sizeof(event.name) - 1] = '\0';
    }
    event.rssi = rssi;
    event.passkey = passkey;
    event.reason = reason;
    config.on_event(&event, config.context);
}

static void emit_input(const doubtech_hid_central_input_t *input)
{
    if (config.on_input) config.on_input(input, config.context);
}

static bool valid_kind(uint8_t kind) { return kind == DTHID_INPUT_KEYBOARD || kind == DTHID_INPUT_MOUSE; }

static bool registry_has_addr(const ble_addr_t *addr)
{
    for (uint8_t kind = DTHID_INPUT_KEYBOARD; kind <= DTHID_INPUT_MOUSE; ++kind) {
        dthid_input_record_t record;
        if (central_registry_get(&registry, (dthid_input_kind_t)kind, &record) && record.addr_type == addr->type &&
            memcmp(record.addr, addr->val, sizeof(record.addr)) == 0)
            return true;
    }
    return false;
}

/* Guest and central bonds share one NimBLE key store and look alike there. */
static bool peer_is_guest(const ble_addr_t *addr) { return doubtech_hid_peer_is_guest(addr->type, addr->val); }

/* Deletes a bond only when it belongs to neither a guest nor a stored input device. */
static void delete_bond_if_ours(const ble_addr_t *addr, const char *why)
{
    if (!central_bond_deletable(peer_is_guest(addr), registry_has_addr(addr))) return;
    int rc = ble_store_util_delete_peer(addr);
    if (rc != 0 && rc != BLE_HS_ENOENT) ESP_LOGW(tag, "could not delete %s bond: %d", why, rc);
}

static uint8_t free_kinds(void)
{
    uint8_t mask = 0;
    dthid_input_record_t record;
    for (uint8_t kind = DTHID_INPUT_KEYBOARD; kind <= DTHID_INPUT_MOUSE; ++kind)
        if (!central_registry_get(&registry, (dthid_input_kind_t)kind, &record)) mask |= CENTRAL_KIND_BIT(kind);
    return mask;
}

static ble_addr_t record_addr(const dthid_input_record_t *record)
{
    ble_addr_t addr = {.type = record->addr_type};
    memcpy(addr.val, record->addr, sizeof(addr.val));
    return addr;
}

/* Copies the registry's names for doubtech_hid_central_stored; host thread,
 * after every registry change. */
static void publish_registry(void)
{
    if (ble_npl_mutex_pend(&stored_lock, BLE_NPL_TIME_FOREVER) != BLE_NPL_OK) return;
    for (uint8_t kind = DTHID_INPUT_KEYBOARD; kind <= DTHID_INPUT_MOUSE; ++kind) {
        dthid_input_record_t record;
        published[kind].stored = central_registry_get(&registry, (dthid_input_kind_t)kind, &record);
        memset(published[kind].name, 0, sizeof(published[kind].name));
        if (published[kind].stored) {
            memcpy(published[kind].name, record.name, sizeof(published[kind].name));
            published[kind].name[sizeof(published[kind].name) - 1] = '\0';
        }
    }
    ble_npl_mutex_release(&stored_lock);
}

static uint32_t now_ms(void) { return ble_npl_time_ticks_to_ms32(ble_npl_time_get()); }

/* Runs one reconnect evaluation on the host loop soon, outside the caller's
 * frame (a queued event is never queued twice). */
static void reconnect_kick(void)
{
    if (started) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &reconnect_event);
}

/* ---- links --------------------------------------------------------------- */

static void *link_arg(const link_t *link)
{
    uintptr_t index = (uintptr_t)(link - links);
    return (void *)(((uintptr_t)link->gen << 8) | index);
}

static link_t *link_from_arg(void *arg)
{
    uintptr_t value = (uintptr_t)arg;
    uintptr_t index = value & 0xFFu;
    if (index >= LINK_COUNT) return NULL;
    link_t *link = &links[index];
    if (link->phase == LINK_FREE || link->gen != (uint16_t)(value >> 8)) return NULL;
    return link;
}

static link_t *link_alloc(void)
{
    for (size_t i = 0; i < LINK_COUNT; ++i) {
        if (links[i].phase != LINK_FREE) continue;
        link_t *link = &links[i];
        memset(link, 0, sizeof(*link));
        link->gen = next_gen++;
        if (next_gen == 0) next_gen = 1;
        link->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return link;
    }
    return NULL;
}

static void pair_end(void)
{
    if (pair.phase == PAIR_IDLE) return;
    ble_hs_cfg.sm_mitm = pair.saved_mitm; /* restore the guest pairing requirement */
    ble_npl_callout_stop(&pair_timer);
    pair.phase = PAIR_IDLE;
    pair.scan_deferred = false;
    pair.link = -1;
    central_scan_clear(&pair.list);
    reconnect_kick(); /* stored devices resume once the window is over */
}

static int start_scan(void)
{
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.itvl = SCAN_ITVL;
    params.window = SCAN_WINDOW;
    params.filter_duplicates = 1; /* active scan (passive = 0) to receive names */
    return ble_gap_disc(own_addr_type, (int32_t)DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS, &params, gap_event, NULL);
}

/* The kind a link stands for: the resolved kind once discovery found it,
 * otherwise the advertised or stored kind. */
static uint8_t link_kind(const link_t *link) { return link->kind ? link->kind : link->kind_hint; }

/* Releases held input downstream if the link was delivering it: resets the
 * machine and emits the lost input exactly once (the machine leaves READY). */
static void link_release_input(link_t *link)
{
    doubtech_hid_central_input_t lost;
    if (central_gatt_disconnect(&link->gatt, &lost)) emit_input(&lost);
}

/* The link is gone (DISCONNECT, failed CONNECT, nothing left to cancel, or a
 * close that never completed). The single place that reports the outcome. */
static void link_closed(link_t *link)
{
    bool was_pairing = link->pairing;
    bool was_ready = link->announced;
    uint8_t kind = link_kind(link);
    link_release_input(link);
    if (was_pairing && link->bonded) delete_bond_if_ours(&link->peer_id, "the unfinished"); /* never a guest's */
    if (link->announced) {
        kind_connected[link->kind] = false;
        emit(DTHID_CENTRAL_DISCONNECTED, 0, link->kind, link->name, 0, 0, 0);
    }
    if (was_pairing && !link->silent)
        emit(DTHID_CENTRAL_PAIR_FAILED, 0, link_kind(link), link->name, 0, 0,
             link->fail_reason ? link->fail_reason : REASON(TIMEOUT));
    link->phase = LINK_FREE;
    link->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (was_pairing) pair_end();
    if (pair.phase == PAIR_SCANNING && pair.scan_deferred && start_scan() == 0) pair.scan_deferred = false;
    if (!was_pairing && valid_kind(kind)) {
        /* A ready device that dropped (asleep) is retried at once (at most once
         * per 30 s); an attempt that never got ready starts the global gap.
         * A forgotten kind is not retried. */
        if (was_ready) central_reconnect_link_lost(&reconnect, kind, now_ms(), esp_random());
        else central_reconnect_attempt_failed(&reconnect, kind, now_ms(), esp_random());
    }
    reconnect_kick(); /* the initiator or a link slot is free again */
}

/* Arms the guard timer for the earliest stored link that is connecting or
 * closing (pairing links are guarded by the window timer). */
static void guard_arm(void)
{
    ble_npl_time_t now = ble_npl_time_get();
    bool any = false;
    ble_npl_time_t wait = 0;
    for (size_t i = 0; i < LINK_COUNT; ++i) {
        const link_t *link = &links[i];
        if (link->pairing || (link->phase != LINK_CONNECTING && link->phase != LINK_CLOSING)) continue;
        int32_t left = (int32_t)(link->guard_at - now);
        ble_npl_time_t ticks = left > 0 ? (ble_npl_time_t)left : 1u;
        if (!any || ticks < wait) wait = ticks;
        any = true;
    }
    if (any) (void)ble_npl_callout_reset(&guard_timer, wait);
    else ble_npl_callout_stop(&guard_timer);
}

static void guard_link(link_t *link, uint32_t ms)
{
    if (link->pairing) return;
    link->guard_at = ble_npl_time_get() + ble_npl_time_ms_to_ticks32(ms);
    guard_arm();
}

/* Fails or cancels a link: releases its input (lost when it was READY, so a
 * forget of a connected device lets the hub drop held keys), stops further
 * notifications, then cancels the connect or terminates the connection. The
 * closing event finishes the cleanup; a link that does not close within
 * CLOSE_GUARD_MS is dropped (a pairing link by the window timer, a stored link
 * by the guard timer, after which the schedule reconnects it), and a terminate
 * that cannot be issued drops the link at once, so the window, sm_mitm and the
 * reconnect schedule never wedge. The device stops counting as connected now. */
static void link_abort(link_t *link, uint8_t reason, bool silent)
{
    if (link->phase == LINK_FREE || link->phase == LINK_CLOSING) return;
    link->fail_reason = reason;
    link->silent = silent;
    link_release_input(link);
    if (link->announced) kind_connected[link->kind] = false; /* DISCONNECTED follows at close */
    link_phase_t was = link->phase;
    link->phase = LINK_CLOSING;
    if (link->pairing) (void)ble_npl_callout_reset(&pair_timer, ble_npl_time_ms_to_ticks32(CLOSE_GUARD_MS));
    else guard_link(link, CLOSE_GUARD_MS);
    if (was == LINK_CONNECTING) {
        if (ble_gap_conn_cancel() != 0) link_closed(link); /* nothing pending: no event follows */
        return;
    }
    int rc = ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(tag, "terminate failed (%d); dropping the link", rc);
        link_closed(link); /* a later event for this slot no longer matches its generation */
    }
}

static void link_ready(link_t *link)
{
    link->phase = LINK_READY;
    link->kind = link->gatt.kind;
    link->kind_hint = link->kind; /* forget and connect_stored match the resolved kind */
    if (link->pairing) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(link->conn_handle, &desc) != 0) {
            (void)central_gatt_disconnect(&link->gatt, NULL); /* no input has flowed yet */
            link_abort(link, REASON(TIMEOUT), false);
            return;
        }
        dthid_input_record_t record;
        memset(&record, 0, sizeof(record));
        memcpy(record.addr, desc.peer_id_addr.val, sizeof(record.addr));
        record.addr_type = desc.peer_id_addr.type;
        record.kind = (dthid_input_kind_t)link->kind;
        memcpy(record.name, link->name, sizeof(record.name));
        central_registry_t next = registry;
        if (!central_registry_set(&next, &record) || central_store_save(store_ns, &next) != ESP_OK) {
            ESP_LOGW(tag, "could not store the paired device");
            (void)central_gatt_disconnect(&link->gatt, NULL); /* no input has flowed yet */
            link_abort(link, REASON(CAPACITY), false);
            return;
        }
        registry = next;
        publish_registry();
        link->pairing = false;
        emit(DTHID_CENTRAL_PAIRED, 0, link->kind, link->name, 0, 0, 0);
        pair_end();
    }
    link->announced = true;
    kind_connected[link->kind] = true;
    emit(DTHID_CENTRAL_CONNECTED, 0, link->kind, link->name, 0, 0, 0);
}

/* ---- GATT client callbacks ---------------------------------------------- */

static central_gatt_error_t error_class(int status)
{
    if (status == BLE_HS_ETIMEOUT) return CG_ERR_TIMEOUT;
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC) ||
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ) ||
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHOR))
        return CG_ERR_AUTH;
    if (status == BLE_HS_ENOMEM || status == BLE_HS_EBUSY) return CG_ERR_BUSY;
    return CG_ERR_OTHER;
}

/* The discovering link a GATT callback belongs to, or NULL for a stale one. */
static link_t *discovering(uint16_t conn_handle, void *arg)
{
    link_t *link = link_from_arg(arg);
    return link && link->phase == LINK_DISCOVERING && link->conn_handle == conn_handle ? link : NULL;
}

/* A failed procedure. ENOTCONN means the link is already closing; its
 * DISCONNECT (or a reconnect attempt's CONNECT) does the reporting. */
static int gatt_failed(link_t *link, int status)
{
    if (status == BLE_HS_ENOTCONN) return 0;
    central_gatt_action_t act;
    central_gatt_error(&link->gatt, error_class(status), &act);
    run(link, act);
    return 0;
}

static int on_service(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service,
                      void *arg)
{
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    central_gatt_action_t act;
    if (error->status == 0 && service) {
        central_gatt_service(&link->gatt, service->start_handle, service->end_handle);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        central_gatt_services_done(&link->gatt, &act);
        run(link, act);
        return 0;
    }
    return gatt_failed(link, error->status);
}

static int on_characteristic(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                             void *arg)
{
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    central_gatt_action_t act;
    if (error->status == 0 && chr) {
        central_gatt_characteristic(&link->gatt, ble_uuid_u16(&chr->uuid.u), chr->def_handle, chr->val_handle,
                                    chr->properties);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        central_gatt_characteristics_done(&link->gatt, &act);
        run(link, act);
        return 0;
    }
    return gatt_failed(link, error->status);
}

static int on_descriptor(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; /* ownership is derived from the characteristic ranges */
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    central_gatt_action_t act;
    if (error->status == 0 && dsc) {
        central_gatt_descriptor(&link->gatt, ble_uuid_u16(&dsc->uuid.u), dsc->handle);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        central_gatt_descriptors_done(&link->gatt, &act);
        run(link, act);
        return 0;
    }
    return gatt_failed(link, error->status);
}

static int on_report_map(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                         void *arg)
{
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    central_gatt_action_t act;
    if (error->status == 0 && attr) {
        uint16_t room = (uint16_t)(sizeof(link->map) - link->map_len);
        uint16_t length = OS_MBUF_PKTLEN(attr->om);
        uint16_t take = length < room ? length : room;
        if (take && os_mbuf_copydata(attr->om, 0, take, link->map + link->map_len) != 0)
            return gatt_failed(link, BLE_HS_EBADDATA);
        link->map_len = (uint16_t)(link->map_len + take);
        if (take == length) return 0; /* a map of exactly 512 bytes ends with EDONE next */
        /* Longer than the walker's 512-byte limit: never parse a truncated prefix. */
        central_gatt_report_map_too_long(&link->gatt, &act);
        run(link, act);
        return 1; /* stop the read */
    }
    /* Some devices answer a blob read at the exact end with an error instead of an empty value. */
    bool ended = error->status == BLE_HS_EDONE ||
                 (link->map_len > 0 && (error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_OFFSET) ||
                                        error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_LONG)));
    if (ended) {
        central_gatt_report_map(&link->gatt, link->map, link->map_len, &act);
        run(link, act);
        return 0;
    }
    return gatt_failed(link, error->status);
}

static int on_report_reference(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                               void *arg)
{
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    if (error->status != 0 || !attr) return gatt_failed(link, error->status ? error->status : BLE_HS_EBADDATA);
    uint8_t value[2] = {0, 0};
    uint16_t length = OS_MBUF_PKTLEN(attr->om);
    if (length > sizeof(value)) length = sizeof(value);
    if (os_mbuf_copydata(attr->om, 0, length, value) != 0) return gatt_failed(link, BLE_HS_EBADDATA);
    central_gatt_action_t act;
    central_gatt_report_reference(&link->gatt, value, length, &act);
    run(link, act);
    return 0;
}

static int on_written(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    link_t *link = discovering(conn_handle, arg);
    if (!link) return 1;
    if (error->status != 0) return gatt_failed(link, error->status);
    central_gatt_action_t act;
    central_gatt_written(&link->gatt, &act);
    run(link, act);
    return 0;
}

/* Performs actions until one is pending on the radio or discovery ends. */
static void run(link_t *link, central_gatt_action_t act)
{
    for (int guard = 0; guard < 8; ++guard) {
        uint16_t handle = link->conn_handle;
        void *arg = link_arg(link);
        int rc = 0;
        if (link->gatt.kind) link->kind = link->gatt.kind; /* resolved after the characteristics */
        switch (act.type) {
        case CG_ACT_NONE: return;
        case CG_ACT_READY: link_ready(link); return;
        case CG_ACT_FAIL:
            ESP_LOGW(tag, "HID discovery failed: reason %u", act.reason);
            link_abort(link, act.reason, false);
            return;
        case CG_ACT_DISCOVER_SERVICE:
            rc = ble_gattc_disc_svc_by_uuid(handle, BLE_UUID16_DECLARE(CENTRAL_UUID_HID_SERVICE), on_service, arg);
            break;
        case CG_ACT_DISCOVER_CHARACTERISTICS:
            rc = ble_gattc_disc_all_chrs(handle, act.start, act.end, on_characteristic, arg);
            break;
        case CG_ACT_DISCOVER_DESCRIPTORS:
            rc = ble_gattc_disc_all_dscs(handle, act.start, act.end, on_descriptor, arg);
            break;
        case CG_ACT_READ_LONG:
            link->map_len = 0;
            rc = ble_gattc_read_long(handle, act.handle, 0, on_report_map, arg);
            break;
        case CG_ACT_READ: rc = ble_gattc_read(handle, act.handle, on_report_reference, arg); break;
        case CG_ACT_WRITE:
            rc = ble_gattc_write_flat(handle, act.handle, act.value, act.value_len, on_written, arg);
            break;
        case CG_ACT_WRITE_NO_RSP:
            rc = ble_gattc_write_no_rsp_flat(handle, act.handle, act.value, act.value_len);
            if (rc == 0) {
                central_gatt_written(&link->gatt, &act);
                continue;
            }
            break;
        }
        if (rc == 0) return;
        central_gatt_error(&link->gatt, error_class(rc), &act);
    }
    link_abort(link, REASON(NOT_SUPPORTED), false); /* unreachable: the machine always settles */
}

/* ---- GAP ------------------------------------------------------------------ */

static void on_connect(link_t *link, const struct ble_gap_event *event)
{
    if (event->connect.status != 0) {
        if (!link->fail_reason) link->fail_reason = REASON(TIMEOUT);
        link_closed(link);
        return;
    }
    link->conn_handle = event->connect.conn_handle;
    if (link->phase == LINK_CLOSING) { /* cancelled or failed while the connect completed */
        (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    /* A fresh link, or NimBLE's automatic reattempt after a failed establishment. */
    (void)central_gatt_disconnect(&link->gatt, NULL);
    link->bonded = false;
    link->phase = LINK_SECURING;
    int rc = ble_gap_security_initiate(link->conn_handle);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(tag, "security start failed: %d", rc);
        link_abort(link, REASON(AUTH_FAILED), false);
    }
}

static void on_enc_change(link_t *link, const struct ble_gap_event *event)
{
    if (link->phase != LINK_SECURING) return; /* a later key refresh changes nothing */
    struct ble_gap_conn_desc desc;
    if (event->enc_change.status != 0 || ble_gap_conn_find(link->conn_handle, &desc) != 0 ||
        !desc.sec_state.encrypted || !desc.sec_state.bonded) {
        ESP_LOGW(tag, "input device security failed: %d", event->enc_change.status);
        link_abort(link, REASON(AUTH_FAILED), false);
        return;
    }
    /* A scanned private address can resolve to a guest's identity (its bond then
     * encrypted this link): never adopt it as an input device, never mark it ours.
     * A stored link is held to the same rule: a stale record never reconnects a guest. */
    if (peer_is_guest(&desc.peer_id_addr)) {
        link_abort(link, REASON(CAPACITY), false);
        return;
    }
    link->bonded = true;
    link->peer_id = desc.peer_id_addr;
    link->phase = LINK_DISCOVERING;
    central_gatt_action_t act;
    central_gatt_begin(&link->gatt, link->kind_hint, link->allowed, &act);
    run(link, act);
}

static void on_passkey(link_t *link, const struct ble_gap_event *event)
{
    if (!link->pairing || link->phase != LINK_SECURING) {
        link_abort(link, REASON(AUTH_FAILED), false); /* never pair outside the owner's window */
        return;
    }
    struct ble_sm_io io;
    memset(&io, 0, sizeof(io));
    io.action = event->passkey.params.action;
    if (io.action == BLE_SM_IOACT_DISP) {
        io.passkey = esp_random() % 1000000u; /* the owner types it on the keyboard */
    } else if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 1; /* the owner selected this device: accept silently, nothing to type */
    } else {
        link_abort(link, REASON(AUTH_FAILED), false);
        return;
    }
    if (ble_sm_inject_io(event->passkey.conn_handle, &io) != 0) {
        link_abort(link, REASON(AUTH_FAILED), false);
        return;
    }
    if (io.action == BLE_SM_IOACT_DISP)
        emit(DTHID_CENTRAL_PASSKEY, 0, link_kind(link), link->name, 0, io.passkey, 0);
}

static int on_repeat_pairing(link_t *link, const struct ble_gap_event *event)
{
    struct ble_gap_conn_desc desc;
    if (link->pairing && link->phase == LINK_SECURING &&
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0 &&
        central_bond_deletable(peer_is_guest(&desc.peer_id_addr), registry_has_addr(&desc.peer_id_addr)) &&
        ble_store_util_delete_peer(&desc.peer_id_addr) == 0)
        return BLE_GAP_REPEAT_PAIRING_RETRY; /* a stale bond of an unlisted, non-guest device */
    link_abort(link, REASON(AUTH_FAILED), false);
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
}

static void on_notify(link_t *link, const struct ble_gap_event *event)
{
    if (link->phase != LINK_READY || event->notify_rx.conn_handle != link->conn_handle) return;
    uint8_t data[CENTRAL_GATT_MAX_REPORT_BYTES];
    uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (length > sizeof(data)) length = sizeof(data);
    if (os_mbuf_copydata(event->notify_rx.om, 0, length, data) != 0) return;
    doubtech_hid_central_input_t input;
    if (central_gatt_notify(&link->gatt, event->notify_rx.attr_handle, data, length, &input)) emit_input(&input);
}

static void on_disconnect(link_t *link, const struct ble_gap_event *event)
{
    if (event->disconnect.conn.conn_handle != link->conn_handle) return;
    if (link->pairing && !link->fail_reason) link->fail_reason = link->bonded ? REASON(TIMEOUT) : REASON(AUTH_FAILED);
    link_closed(link); /* lost input (if still READY), bond cleanup and events */
}

static void on_scan_report(const struct ble_gap_event *event)
{
    if (pair.phase != PAIR_SCANNING) return;
    central_adv_info_t info;
    central_adv_parse(event->disc.data, event->disc.length_data, &info);
    int index = central_scan_offer(&pair.list, event->disc.addr.val, event->disc.addr.type, event->disc.rssi, &info);
    if (index < 0) return;
    const central_scan_entry_t *entry = &pair.list.entries[index];
    emit(DTHID_CENTRAL_SCAN_RESULT, (uint8_t)index, entry->kind, entry->name, entry->rssi, 0, 0);
}

static void pair_fail_window(uint8_t reason)
{
    if (pair.phase == PAIR_SCANNING && !pair.scan_deferred) (void)ble_gap_disc_cancel();
    emit(DTHID_CENTRAL_PAIR_FAILED, 0, 0, NULL, 0, 0, reason);
    pair_end();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        on_scan_report(event);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (pair.phase == PAIR_SCANNING && !pair.scan_deferred) pair_fail_window(REASON(TIMEOUT));
        return 0;
    }
    link_t *link = link_from_arg(arg);
    if (!link) {
        /* No owner (a dropped or reused slot): this callback only serves central
         * links, so terminate any such connection that is still talking to us.
         * Guest links never reach this callback. */
        uint16_t orphan = BLE_HS_CONN_HANDLE_NONE;
        switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) orphan = event->connect.conn_handle;
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION: orphan = event->passkey.conn_handle; break;
        case BLE_GAP_EVENT_ENC_CHANGE: orphan = event->enc_change.conn_handle; break;
        case BLE_GAP_EVENT_NOTIFY_RX: orphan = event->notify_rx.conn_handle; break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: orphan = event->repeat_pairing.conn_handle; break;
        default: break;
        }
        if (orphan != BLE_HS_CONN_HANDLE_NONE) (void)ble_gap_terminate(orphan, BLE_ERR_REM_USER_CONN_TERM);
        return event->type == BLE_GAP_EVENT_REPEAT_PAIRING ? BLE_GAP_REPEAT_PAIRING_IGNORE : 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: on_connect(link, event); return 0;
    case BLE_GAP_EVENT_DISCONNECT: on_disconnect(link, event); return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: on_enc_change(link, event); return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: on_passkey(link, event); return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: return on_repeat_pairing(link, event);
    case BLE_GAP_EVENT_NOTIFY_RX: on_notify(link, event); return 0;
    default: return 0;
    }
}

/* ---- host-thread operations ---------------------------------------------- */

static void pair_timeout_on_host(struct ble_npl_event *event)
{
    (void)event;
    if (pair.phase == PAIR_SCANNING) pair_fail_window(REASON(TIMEOUT));
    else if (pair.phase == PAIR_LINKING && pair.link >= 0) {
        link_t *link = &links[pair.link];
        if (link->phase != LINK_CLOSING) {
            link_abort(link, REASON(TIMEOUT), false); /* rearms the timer as a close guard */
            return;
        }
        /* The close never completed: retry the terminate once and drop the link so
         * the window ends and sm_mitm is restored. Later events miss its generation. */
        if (link->conn_handle != BLE_HS_CONN_HANDLE_NONE)
            (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (!link->fail_reason) link->fail_reason = REASON(TIMEOUT);
        link_closed(link);
    }
}

/* A stored link still connecting past its attempt, or closing past the close
 * guard: give up on it (cancel or terminate once more) and drop it, so the
 * schedule can reconnect. Later events for the slot miss its generation. */
static void guard_on_host(struct ble_npl_event *event)
{
    (void)event;
    ble_npl_time_t now = ble_npl_time_get();
    for (size_t i = 0; i < LINK_COUNT; ++i) {
        link_t *link = &links[i];
        if (link->pairing || (link->phase != LINK_CONNECTING && link->phase != LINK_CLOSING)) continue;
        if ((int32_t)(now - link->guard_at) < 0) continue;
        ESP_LOGW(tag, "input link did not %s in time; dropping it",
                 link->phase == LINK_CONNECTING ? "connect" : "close");
        if (link->phase == LINK_CONNECTING) {
            (void)ble_gap_conn_cancel();
        } else if (link->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (!link->fail_reason) link->fail_reason = REASON(TIMEOUT);
        link_closed(link);
    }
    guard_arm();
}

static void reconnect_facts(central_reconnect_facts_t *f)
{
    memset(f, 0, sizeof(*f));
    for (uint8_t kind = DTHID_INPUT_KEYBOARD; kind <= DTHID_INPUT_MOUSE; ++kind) {
        dthid_input_record_t record;
        f->stored[kind] = central_registry_get(&registry, (dthid_input_kind_t)kind, &record);
        if (f->stored[kind]) {
            ble_addr_t addr = record_addr(&record);
            f->guest[kind] = peer_is_guest(&addr);
        }
    }
    for (size_t i = 0; i < LINK_COUNT; ++i) {
        if (links[i].phase == LINK_FREE) continue;
        f->links++;
        if (links[i].phase == LINK_CONNECTING) f->connecting = true;
        uint8_t kind = link_kind(&links[i]);
        if (valid_kind(kind)) f->linked[kind] = true;
    }
    if (ble_gap_conn_active()) f->connecting = true; /* one initiator, whoever holds it */
    f->pairing = pair.phase != PAIR_IDLE;
    /* Guests keep their full share: the central only uses what is left. */
    f->link_budget = central_reconnect_link_budget(CONFIG_BT_NIMBLE_MAX_CONNECTIONS, DOUBTECH_HID_MAX_CONNECTIONS,
                                                   LINK_COUNT);
}

/* One schedule evaluation (timer, kick): start at most one attempt, then arm
 * the timer for the next step or stop it. A connect that cannot start (busy
 * controller, no memory, host not synced) is an ordinary failed attempt. */
static void reconnect_on_host(struct ble_npl_event *event)
{
    (void)event;
    if (!started) return;
    for (int round = 0; round < 3; ++round) {
        central_reconnect_facts_t facts;
        reconnect_facts(&facts);
        uint32_t now = now_ms();
        central_reconnect_action_t act = central_reconnect_next(&reconnect, now, &facts);
        if (act.step == CENTRAL_RECONNECT_NONE) {
            ble_npl_callout_stop(&reconnect_timer);
            return;
        }
        if (act.step == CENTRAL_RECONNECT_WAIT) {
            ble_npl_time_t ticks = ble_npl_time_ms_to_ticks32(act.delay_ms);
            (void)ble_npl_callout_reset(&reconnect_timer, ticks ? ticks : 1u);
            return;
        }
        esp_err_t result = central_connect_stored(act.kind);
        if (result == ESP_OK) {
            central_reconnect_attempt_started(&reconnect, act.kind, now);
        } else {
            ESP_LOGD(tag, "reconnect of kind %u not started: %d", act.kind, result);
            central_reconnect_attempt_failed(&reconnect, act.kind, now, esp_random());
        }
    }
    (void)ble_npl_callout_reset(&reconnect_timer, ble_npl_time_ms_to_ticks32(CENTRAL_RECONNECT_GAP_MS));
}

static bool infer_address_type(void) { return ble_hs_id_infer_auto(0, &own_addr_type) == 0; }

static esp_err_t pair_begin_on_host(void)
{
    if (!doubtech_hid_host_synced()) return ESP_ERR_INVALID_STATE;
    if (pair.phase != PAIR_IDLE || doubtech_hid_pairing_active()) {
        emit(DTHID_CENTRAL_PAIR_FAILED, 0, 0, NULL, 0, 0, REASON(BUSY));
        return ESP_ERR_INVALID_STATE;
    }
    if (free_kinds() == 0) {
        emit(DTHID_CENTRAL_PAIR_FAILED, 0, 0, NULL, 0, 0, REASON(CAPACITY));
        return ESP_ERR_INVALID_STATE;
    }
    if (!infer_address_type()) return ESP_FAIL;
    pair.saved_mitm = ble_hs_cfg.sm_mitm;
    ble_hs_cfg.sm_mitm = 0; /* most mice are Just Works; keyboards still get a passkey */
    pair.phase = PAIR_SCANNING;
    pair.link = -1;
    pair.scan_deferred = false;
    central_scan_clear(&pair.list);
    /* A stored-device connect attempt holds the initiator; give it up for the window. */
    for (size_t i = 0; i < LINK_COUNT; ++i)
        if (links[i].phase == LINK_CONNECTING && !links[i].pairing) {
            link_abort(&links[i], 0, true);
            pair.scan_deferred = links[i].phase != LINK_FREE;
        }
    if (!pair.scan_deferred) {
        int rc = start_scan();
        if (rc != 0) {
            ESP_LOGW(tag, "scan start failed: %d", rc);
            pair_fail_window(REASON(BUSY));
            return ESP_FAIL;
        }
    }
    if (ble_npl_callout_reset(&pair_timer, ble_npl_time_ms_to_ticks32(DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS)) !=
        BLE_NPL_OK) {
        pair_fail_window(REASON(BUSY));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void pair_cancel_on_host(void)
{
    if (pair.phase == PAIR_SCANNING) {
        if (!pair.scan_deferred) (void)ble_gap_disc_cancel();
        pair_end();
    } else if (pair.phase == PAIR_LINKING && pair.link >= 0) {
        link_abort(&links[pair.link], 0, true); /* link_closed ends the window */
    }
}

static esp_err_t pair_select_on_host(uint8_t index)
{
    if (pair.phase != PAIR_SCANNING) return ESP_ERR_INVALID_STATE;
    if (index >= pair.list.count) return ESP_ERR_INVALID_ARG;
    const central_scan_entry_t entry = pair.list.entries[index];
    ble_addr_t addr = {.type = entry.addr_type};
    memcpy(addr.val, entry.addr, sizeof(addr.val));
    uint8_t allowed = free_kinds();
    /* Guests are never connected or bonded; a stored address or a full kind needs forget first. */
    uint8_t refused = central_scan_select_reason(&entry, allowed, registry_has_addr(&addr), peer_is_guest(&addr));
    if (refused) {
        pair_fail_window(refused);
        return ESP_ERR_INVALID_STATE;
    }
    link_t *link = pair.scan_deferred ? NULL : link_alloc();
    if (!link) {
        pair_fail_window(REASON(BUSY));
        return ESP_ERR_INVALID_STATE;
    }
    (void)ble_gap_disc_cancel(); /* the initiator cannot start while scanning */
    /* A stale local bond for this unlisted, non-guest address would make
     * encryption fail forever; pairing starts from no keys. */
    delete_bond_if_ours(&addr, "a stale");
    link->pairing = true;
    link->kind_hint = entry.kind;
    link->allowed = allowed;
    link->addr = addr;
    memcpy(link->name, entry.name, sizeof(link->name));
    link->phase = LINK_CONNECTING;
    int rc = ble_gap_connect(own_addr_type, &addr, CONNECT_TIMEOUT_MS, &conn_params, gap_event, link_arg(link));
    if (rc != 0) {
        ESP_LOGW(tag, "connect failed: %d", rc);
        link->phase = LINK_FREE;
        emit(DTHID_CENTRAL_PAIR_FAILED, 0, entry.kind, entry.name, 0, 0,
             rc == BLE_HS_ENOMEM ? REASON(CAPACITY) : REASON(BUSY));
        pair_end();
        return ESP_FAIL;
    }
    pair.phase = PAIR_LINKING;
    pair.link = (int8_t)(link - links);
    if (ble_npl_callout_reset(&pair_timer, ble_npl_time_ms_to_ticks32(PAIR_COMPLETE_MS)) != BLE_NPL_OK)
        link_abort(link, REASON(BUSY), false);
    return ESP_OK;
}

static esp_err_t forget_on_host(uint8_t kind)
{
    if (!valid_kind(kind)) return ESP_ERR_INVALID_ARG;
    dthid_input_record_t record;
    if (!central_registry_get(&registry, (dthid_input_kind_t)kind, &record)) return ESP_ERR_NOT_FOUND;
    central_registry_t next = registry;
    (void)central_registry_forget(&next, (dthid_input_kind_t)kind);
    esp_err_t result = central_store_save(store_ns, &next);
    if (result != ESP_OK) return result;
    registry = next;
    publish_registry();
    for (size_t i = 0; i < LINK_COUNT; ++i)
        if (links[i].phase != LINK_FREE && !links[i].pairing && link_kind(&links[i]) == kind)
            link_abort(&links[i], 0, true); /* reports the lost input, then DISCONNECTED */
    reconnect_kick(); /* the kind is no longer stored: its retries stop */
    ble_addr_t addr = record_addr(&record);
    /* A combo device keeps its bond for the other kind; a guest's bond is never touched. */
    if (central_bond_deletable(peer_is_guest(&addr), registry_has_addr(&addr))) {
        int rc = ble_store_util_delete_peer(&addr);
        if (rc != 0 && rc != BLE_HS_ENOENT) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t central_connect_stored(uint8_t kind)
{
    if (!started || !doubtech_hid_host_synced()) return ESP_ERR_INVALID_STATE;
    if (!valid_kind(kind)) return ESP_ERR_INVALID_ARG;
    if (pair.phase != PAIR_IDLE) return ESP_ERR_INVALID_STATE;
    dthid_input_record_t record;
    if (!central_registry_get(&registry, (dthid_input_kind_t)kind, &record)) return ESP_ERR_NOT_FOUND;
    ble_addr_t addr = record_addr(&record);
    if (peer_is_guest(&addr)) return ESP_ERR_INVALID_STATE; /* a stale record: never connect a guest */
    if (ble_gap_conn_active()) return ESP_ERR_INVALID_STATE; /* one outgoing attempt at a time */
    for (size_t i = 0; i < LINK_COUNT; ++i)
        if (links[i].phase != LINK_FREE && (links[i].phase == LINK_CONNECTING || link_kind(&links[i]) == kind))
            return ESP_ERR_INVALID_STATE;
    if (!infer_address_type()) return ESP_FAIL;
    link_t *link = link_alloc();
    if (!link) return ESP_ERR_NO_MEM;
    link->kind_hint = kind;
    link->allowed = CENTRAL_KIND_BIT(kind);
    link->addr = addr;
    memcpy(link->name, record.name, sizeof(link->name));
    link->name[sizeof(link->name) - 1] = '\0';
    link->phase = LINK_CONNECTING;
    int rc = ble_gap_connect(own_addr_type, &link->addr, CONNECT_TIMEOUT_MS, &conn_params, gap_event, link_arg(link));
    if (rc != 0) {
        link->phase = LINK_FREE;
        if (rc == BLE_HS_ENOMEM) return ESP_ERR_NO_MEM;
        return rc == BLE_HS_EALREADY || rc == BLE_HS_EBUSY || rc == BLE_HS_EDONE ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    /* The attempt's CONNECT event normally ends it; if that never arrives the
     * guard drops the link so the schedule is never stuck behind it. */
    guard_link(link, CENTRAL_RECONNECT_ATTEMPT_MS + CLOSE_GUARD_MS);
    return ESP_OK;
}

/* ---- bounded host-loop request ------------------------------------------- */

static esp_err_t execute(rpc_op_t op, uint8_t arg)
{
    switch (op) {
    case OP_PAIR_BEGIN: return pair_begin_on_host();
    case OP_PAIR_CANCEL: pair_cancel_on_host(); return ESP_OK;
    case OP_PAIR_SELECT: return pair_select_on_host(arg);
    case OP_FORGET: return forget_on_host(arg);
    }
    return ESP_ERR_INVALID_ARG;
}

static void rpc_on_host(struct ble_npl_event *event)
{
    (void)event;
    if (ble_npl_mutex_pend(&rpc.lock, BLE_NPL_TIME_FOREVER) != BLE_NPL_OK) return;
    bool run_it = rpc.busy && !rpc.finished && (int32_t)(rpc.deadline - ble_npl_time_get()) > 0;
    bool pending = rpc.busy && !rpc.finished;
    rpc_op_t op = rpc.op;
    uint8_t arg = rpc.arg;
    ble_npl_mutex_release(&rpc.lock);
    if (!pending) return;
    esp_err_t result = run_it ? execute(op, arg) : ESP_ERR_TIMEOUT; /* outside the lock */
    if (ble_npl_mutex_pend(&rpc.lock, BLE_NPL_TIME_FOREVER) != BLE_NPL_OK) return;
    rpc.result = result;
    rpc.finished = true;
    if (rpc.abandoned) rpc.busy = rpc.abandoned = false;
    ble_npl_mutex_release(&rpc.lock);
    ble_npl_sem_release(&rpc.done);
}

static esp_err_t request(rpc_op_t op, uint8_t arg)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    ble_npl_time_t wait = ble_npl_time_ms_to_ticks32(RPC_TIMEOUT_MS);
    if (ble_npl_mutex_pend(&rpc.lock, wait) != BLE_NPL_OK) return ESP_ERR_TIMEOUT;
    if (rpc.busy) { /* an abandoned request has not run yet */
        ble_npl_mutex_release(&rpc.lock);
        return ESP_ERR_INVALID_STATE;
    }
    while (ble_npl_sem_pend(&rpc.done, 0) == BLE_NPL_OK) {
    }
    rpc.busy = true;
    rpc.finished = rpc.abandoned = false;
    rpc.op = op;
    rpc.arg = arg;
    rpc.result = ESP_ERR_TIMEOUT;
    rpc.deadline = ble_npl_time_get() + wait;
    ble_npl_mutex_release(&rpc.lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &rpc.event);
    (void)ble_npl_sem_pend(&rpc.done, wait);
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (ble_npl_mutex_pend(&rpc.lock, BLE_NPL_TIME_FOREVER) != BLE_NPL_OK) return result;
    if (rpc.finished) {
        result = rpc.result;
        rpc.busy = false;
    } else {
        rpc.abandoned = true; /* the host clears busy when it gets to it */
    }
    ble_npl_mutex_release(&rpc.lock);
    return result;
}

/* ---- public API ----------------------------------------------------------- */

esp_err_t doubtech_hid_central_start(const doubtech_hid_central_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (started || !doubtech_hid_host_synced()) return ESP_ERR_INVALID_STATE;
    const char *ns = cfg->store_namespace ? cfg->store_namespace : DTHID_INPUTS_DEFAULT_NAMESPACE;
    if (strlen(ns) > DTHID_INPUTS_NAMESPACE_MAX_CHARS) return ESP_ERR_INVALID_ARG;
    central_registry_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    esp_err_t result = central_store_load(ns, &loaded);
    if (result != ESP_OK) return result;
    if (ble_npl_mutex_init(&rpc.lock) != BLE_NPL_OK || ble_npl_sem_init(&rpc.done, 0) != BLE_NPL_OK ||
        ble_npl_mutex_init(&stored_lock) != BLE_NPL_OK)
        return ESP_FAIL;
    ble_npl_event_init(&rpc.event, rpc_on_host, NULL);
    ble_npl_event_init(&reconnect_event, reconnect_on_host, NULL);
    struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
    if (ble_npl_callout_init(&pair_timer, queue, pair_timeout_on_host, NULL) != 0 ||
        ble_npl_callout_init(&reconnect_timer, queue, reconnect_on_host, NULL) != 0 ||
        ble_npl_callout_init(&guard_timer, queue, guard_on_host, NULL) != 0)
        return ESP_FAIL;
    memcpy(store_ns, ns, strlen(ns) + 1);
    config = *cfg;
    config.store_namespace = store_ns;
    registry = loaded;
    publish_registry();
    pair.phase = PAIR_IDLE;
    pair.link = -1;
    central_reconnect_init(&reconnect, now_ms()); /* stored devices are due at once */
    started = true;
    reconnect_kick(); /* the first evaluation runs on the host loop */
    return ESP_OK;
}

esp_err_t doubtech_hid_central_pair_begin(void) { return request(OP_PAIR_BEGIN, 0); }

esp_err_t doubtech_hid_central_pair_cancel(void) { return request(OP_PAIR_CANCEL, 0); }

esp_err_t doubtech_hid_central_pair_select(uint8_t index) { return request(OP_PAIR_SELECT, index); }

esp_err_t doubtech_hid_central_forget(uint8_t kind)
{
    if (!valid_kind(kind)) return ESP_ERR_INVALID_ARG;
    return request(OP_FORGET, kind);
}

bool doubtech_hid_central_connected(uint8_t kind) { return valid_kind(kind) && kind_connected[kind]; }

bool doubtech_hid_central_stored(uint8_t kind, char name[DOUBTECH_HID_CENTRAL_NAME_BYTES])
{
    if (name) name[0] = '\0';
    if (!started || !valid_kind(kind)) return false;
    /* Held only for a copy on either side, so waiting never blocks for long; a
     * timed-out read would misreport a stored device as absent. */
    if (ble_npl_mutex_pend(&stored_lock, BLE_NPL_TIME_FOREVER) != BLE_NPL_OK) return false;
    bool stored = published[kind].stored;
    if (stored && name) memcpy(name, published[kind].name, DOUBTECH_HID_CENTRAL_NAME_BYTES);
    ble_npl_mutex_release(&stored_lock);
    return stored;
}
