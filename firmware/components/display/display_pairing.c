/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the USB-independent pairing state for the local touch display.
 * It binds approval or rejection to the one live six-digit challenge and
 * clears comparison data after reply, expiry, cancellation, or release. */
#include "display_pairing.h"
#include <string.h>

void kvm_display_pairing_started(kvm_display_pairing_t *p, bool local_owner,
                                 uint64_t now_ms)
{
    if (!p) return;
    *p = (kvm_display_pairing_t){.local_owner = local_owner,
                                  .state = KVM_DISPLAY_PAIRING_WAITING,
                                  .deadline_ms = now_ms + 60000u};
}

bool kvm_display_pairing_event(kvm_display_pairing_t *p,
                               kvm_display_pairing_state_t state, uint32_t challenge_id,
                               uint32_t number, uint64_t deadline_ms, uint64_t now_ms)
{
    if (!p) return false;
    if (state == KVM_DISPLAY_PAIRING_CHALLENGE) {
        if (p->state != KVM_DISPLAY_PAIRING_WAITING || !challenge_id ||
            number > 999999u || deadline_ms <= now_ms ||
            deadline_ms > p->deadline_ms) return false;
        p->challenge_id = challenge_id;
        p->number = number;
        p->deadline_ms = deadline_ms;
        p->state = state;
        return true;
    }
    if (state == KVM_DISPLAY_PAIRING_CLOSED) {
        kvm_display_pairing_clear(p);
        return true;
    }
    if (state != KVM_DISPLAY_PAIRING_WAITING &&
        state != KVM_DISPLAY_PAIRING_REJECTED &&
        state != KVM_DISPLAY_PAIRING_CAPACITY &&
        state != KVM_DISPLAY_PAIRING_TIMEOUT) return false;
    p->challenge_id = 0;
    p->number = 0;
    p->state = state;
    if (state == KVM_DISPLAY_PAIRING_WAITING) p->deadline_ms = deadline_ms;
    return true;
}

bool kvm_display_pairing_accept(const kvm_display_pairing_t *p,
                                kvm_display_pair_request_t request, uint64_t now_ms)
{
    if (!p) return false;
    switch (request.action) {
    case KVM_DISPLAY_PAIR_BEGIN:
        return !request.challenge_id &&
            (p->state == KVM_DISPLAY_PAIRING_CLOSED ||
             p->state == KVM_DISPLAY_PAIRING_REJECTED ||
             p->state == KVM_DISPLAY_PAIRING_TIMEOUT);
    case KVM_DISPLAY_PAIR_CANCEL:
        return p->local_owner && !request.challenge_id &&
            p->state == KVM_DISPLAY_PAIRING_WAITING &&
            p->deadline_ms > now_ms;
    case KVM_DISPLAY_PAIR_APPROVE:
    case KVM_DISPLAY_PAIR_REJECT:
        return p->state == KVM_DISPLAY_PAIRING_CHALLENGE && p->challenge_id &&
            request.challenge_id == p->challenge_id && p->number <= 999999u &&
            p->deadline_ms > now_ms;
    default: return false;
    }
}

void kvm_display_pairing_replied(kvm_display_pairing_t *p, bool approved)
{
    if (!p) return;
    p->challenge_id = 0;
    p->number = 0;
    p->state = approved ? KVM_DISPLAY_PAIRING_WAITING : KVM_DISPLAY_PAIRING_REJECTED;
}

void kvm_display_pairing_expire(kvm_display_pairing_t *p, uint64_t now_ms)
{
    if (!p || (p->state != KVM_DISPLAY_PAIRING_WAITING &&
               p->state != KVM_DISPLAY_PAIRING_CHALLENGE) ||
        p->deadline_ms > now_ms) return;
    p->challenge_id = 0;
    p->number = 0;
    p->state = KVM_DISPLAY_PAIRING_TIMEOUT;
}

void kvm_display_pairing_clear(kvm_display_pairing_t *p)
{
    if (p) memset(p, 0, sizeof(*p));
}

/* ---- Input-device pairing ---- */

/* Copies a possibly unterminated fixed-size name. */
static void copy_name(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    size_t n = 0;
    while (n < src_size && n + 1 < dst_size && src[n]) ++n;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void input_close(kvm_display_input_pairing_t *p)
{
    kvm_display_input_ops_t ops = p->ops;
    uint32_t window = p->window;
    uint64_t busy = p->guest_busy_until_ms;
    memset(p, 0, sizeof(*p));
    p->ops = ops;
    p->window = window;
    p->guest_busy_until_ms = busy;
}

static void input_result(kvm_display_input_pairing_t *p, uint8_t reason, bool refused,
                         uint64_t now_ms)
{
    p->state = KVM_DISPLAY_INPUT_RESULT;
    p->connecting = false;
    p->refused = refused;
    p->reason = reason;
    p->busy_cause = KVM_DISPLAY_INPUT_BUSY_CENTRAL;
    p->passkey = 0;
    p->deadline_ms = now_ms + KVM_DISPLAY_INPUT_RESULT_MS;
    memset(p->scan, 0, sizeof(p->scan));
}

static bool input_window_open(const kvm_display_input_pairing_t *p)
{
    return p->state == KVM_DISPLAY_INPUT_SCANNING || p->state == KVM_DISPLAY_INPUT_PASSKEY;
}

static bool input_cancel_confirmed(kvm_display_input_pairing_t *p)
{
    return !p->ops.cancel || p->ops.cancel(p->ops.context);
}

/* Ends the central window. A cancel the central did not confirm (its RPC
 * timed out, or an abandoned request still holds the host loop) leaves the
 * window possibly open: hold the guest gate and retry, until a confirmed
 * cancel or until the central's own timer has surely ended it. Requests are
 * serialized on the host loop, so a confirmed cancel always ran after any
 * abandoned begin or select. */
static void input_stop(kvm_display_input_pairing_t *p, uint64_t now_ms,
                       uint64_t window_deadline_ms)
{
    if (input_cancel_confirmed(p)) {
        p->stopping = false;
        return;
    }
    p->stopping = true;
    p->stop_deadline_ms = (window_deadline_ms > now_ms ? window_deadline_ms : now_ms) +
                          KVM_DISPLAY_INPUT_STOP_MARGIN_MS;
    p->cancel_retry_ms = now_ms + KVM_DISPLAY_INPUT_CANCEL_RETRY_MS;
}

static void input_retry_stop(kvm_display_input_pairing_t *p, uint64_t now_ms)
{
    if (!p->stopping) return;
    if (now_ms >= p->stop_deadline_ms) p->stopping = false; /* the central's timer ended it */
    else if (input_cancel_confirmed(p)) p->stopping = false;
    else p->cancel_retry_ms = now_ms + KVM_DISPLAY_INPUT_CANCEL_RETRY_MS;
}

void kvm_display_input_pairing_init(kvm_display_input_pairing_t *p, kvm_display_input_ops_t ops)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->ops = ops;
}

bool kvm_display_input_pairing_request(kvm_display_input_pairing_t *p,
                                       kvm_display_input_request_t request, uint64_t now_ms,
                                       bool host_armed, bool guest_pairing)
{
    if (!p) return false;
    switch (request.action) {
    case KVM_DISPLAY_INPUT_PAIR_BEGIN: {
        if (p->state != KVM_DISPLAY_INPUT_CLOSED) return false;
        input_close(p);
        if (++p->window == 0) p->window = 1;
        if (host_armed || guest_pairing) {
            /* Never while the PC drives a guest or beside a guest window. */
            input_result(p, KVM_DISPLAY_INPUT_REASON_BUSY, false, now_ms);
            p->busy_cause = host_armed ? KVM_DISPLAY_INPUT_BUSY_HOST : KVM_DISPLAY_INPUT_BUSY_GUEST;
            return false;
        }
        kvm_display_input_op_t op = p->ops.begin ? p->ops.begin(p->ops.context)
                                                 : KVM_DISPLAY_INPUT_OP_REFUSED;
        if (op != KVM_DISPLAY_INPUT_OP_OK) {
            /* The central reports its own reason (busy or capacity) as an event. */
            input_result(p, KVM_DISPLAY_INPUT_REASON_BUSY, true, now_ms);
            /* A timed-out begin may still open the window later (sm_mitm=0). */
            if (op == KVM_DISPLAY_INPUT_OP_UNKNOWN)
                input_stop(p, now_ms, now_ms + KVM_DISPLAY_INPUT_WINDOW_MS);
            return false;
        }
        p->state = KVM_DISPLAY_INPUT_SCANNING;
        p->deadline_ms = now_ms + KVM_DISPLAY_INPUT_WINDOW_MS;
        return true;
    }
    case KVM_DISPLAY_INPUT_PAIR_SELECT: {
        if (p->state != KVM_DISPLAY_INPUT_SCANNING || p->connecting ||
            request.window != p->window || request.scan_index >= KVM_DISPLAY_INPUT_SCAN_MAX)
            return false;
        const kvm_display_input_scan_t *entry = &p->scan[request.scan_index];
        if (!entry->used) return false;
        p->kind = entry->kind;
        copy_name(p->name, sizeof(p->name), entry->name, sizeof(entry->name));
        kvm_display_input_op_t op = p->ops.select ? p->ops.select(p->ops.context, request.scan_index)
                                                  : KVM_DISPLAY_INPUT_OP_REFUSED;
        if (op != KVM_DISPLAY_INPUT_OP_OK) {
            /* Make sure the window is over; a timed-out select may still link
               later under a fresh central timer. */
            input_result(p, KVM_DISPLAY_INPUT_REASON_BUSY, true, now_ms);
            input_stop(p, now_ms, now_ms + KVM_DISPLAY_INPUT_WINDOW_MS);
            return false;
        }
        p->connecting = true;
        p->deadline_ms = now_ms + KVM_DISPLAY_INPUT_WINDOW_MS;
        memset(p->scan, 0, sizeof(p->scan));
        return true;
    }
    case KVM_DISPLAY_INPUT_PAIR_CANCEL:
        if (p->state == KVM_DISPLAY_INPUT_CLOSED || request.window != p->window) return false;
        if (input_window_open(p)) {
            uint64_t window_deadline = p->deadline_ms;
            input_result(p, KVM_DISPLAY_INPUT_REASON_CANCELLED, false, now_ms);
            input_stop(p, now_ms, window_deadline);
        } else if (p->stopping) {
            input_retry_stop(p, now_ms); /* the owner asked again: retry now */
        }
        if (p->stopping) return false; /* not closed until the central confirms */
        input_close(p);
        return true;
    default: return false;
    }
}

void kvm_display_input_pairing_event(kvm_display_input_pairing_t *p,
                                     const kvm_display_input_event_t *e, uint64_t now_ms)
{
    if (!p || !e || p->state == KVM_DISPLAY_INPUT_CLOSED) return;
    switch (e->type) {
    case KVM_DISPLAY_INPUT_EVENT_SCAN: {
        if (p->state != KVM_DISPLAY_INPUT_SCANNING || p->connecting ||
            e->index >= KVM_DISPLAY_INPUT_SCAN_MAX) return;
        kvm_display_input_scan_t *entry = &p->scan[e->index];
        entry->used = true;
        entry->index = e->index;
        entry->kind = e->kind;
        entry->rssi = e->rssi;
        copy_name(entry->name, sizeof(entry->name), e->name, sizeof(e->name));
        return;
    }
    case KVM_DISPLAY_INPUT_EVENT_PASSKEY:
        if (!input_window_open(p) || !p->connecting || e->passkey > 999999u) return;
        p->state = KVM_DISPLAY_INPUT_PASSKEY;
        p->passkey = e->passkey;
        if (e->kind) p->kind = e->kind;
        return;
    case KVM_DISPLAY_INPUT_EVENT_PAIRED:
        /* Also over a result: a device that made it just before the UI's own
           deadline is paired, and that is what the owner must see. */
        if (e->kind) p->kind = e->kind;
        if (e->name[0]) copy_name(p->name, sizeof(p->name), e->name, sizeof(e->name));
        input_result(p, 0, false, now_ms);
        return;
    case KVM_DISPLAY_INPUT_EVENT_FAILED: {
        uint8_t reason = e->reason >= 1 && e->reason <= 5 ? e->reason
                                                          : KVM_DISPLAY_INPUT_REASON_AUTH_FAILED;
        if (input_window_open(p)) input_result(p, reason, false, now_ms);
        else if (p->state == KVM_DISPLAY_INPUT_RESULT && p->refused) {
            p->reason = reason; /* the central's own reason for the refusal */
            p->refused = false;
        }
        return;
    }
    default: return;
    }
}

void kvm_display_input_pairing_tick(kvm_display_input_pairing_t *p, uint64_t now_ms,
                                    bool host_armed)
{
    if (!p) return;
    if (input_window_open(p)) {
        uint64_t window_deadline = p->deadline_ms;
        if (host_armed) {
            /* The PC armed a guest mid-window: the host route wins. An open
               but unarmed session leaves the window alone. */
            input_result(p, KVM_DISPLAY_INPUT_REASON_BUSY, false, now_ms);
            p->busy_cause = KVM_DISPLAY_INPUT_BUSY_HOST;
            input_stop(p, now_ms, window_deadline);
        } else if (now_ms >= window_deadline) {
            input_result(p, KVM_DISPLAY_INPUT_REASON_TIMEOUT, false, now_ms);
            input_stop(p, now_ms, window_deadline);
        }
    } else if (p->state == KVM_DISPLAY_INPUT_RESULT) {
        if (p->stopping && (now_ms >= p->cancel_retry_ms || now_ms >= p->stop_deadline_ms))
            input_retry_stop(p, now_ms);
        if (!p->stopping && now_ms >= p->deadline_ms) input_close(p);
    }
}

bool kvm_display_input_pairing_blocks_guest(const kvm_display_input_pairing_t *p)
{
    return p && p->state != KVM_DISPLAY_INPUT_CLOSED; /* RESULT lasts while stopping */
}

void kvm_display_input_pairing_guest_refused(kvm_display_input_pairing_t *p, uint64_t now_ms)
{
    if (p) p->guest_busy_until_ms = now_ms + KVM_DISPLAY_INPUT_GUEST_BUSY_MS;
}

static bool scan_before(const kvm_display_input_scan_t *a, const kvm_display_input_scan_t *b)
{
    return a->rssi != b->rssi ? a->rssi > b->rssi : a->index < b->index;
}

void kvm_display_input_pairing_status(const kvm_display_input_pairing_t *p, uint64_t now_ms,
                                      kvm_display_status_t *s)
{
    if (!p || !s) return;
    s->input_pair_state = (uint8_t)p->state;
    s->input_pair_count = 0;
    memset(s->input_pair_rows, 0, sizeof(s->input_pair_rows));
    memset(s->input_pair_index, 0, sizeof(s->input_pair_index));
    s->input_pair_passkey = p->state == KVM_DISPLAY_INPUT_PASSKEY ? p->passkey : 0;
    s->input_pair_reason = p->state == KVM_DISPLAY_INPUT_RESULT ? p->reason : 0;
    s->input_pair_kind = p->kind;
    s->input_pair_connecting = p->connecting;
    s->input_pair_guest_busy = p->state != KVM_DISPLAY_INPUT_CLOSED &&
                               now_ms < p->guest_busy_until_ms;
    copy_name(s->input_pair_name, sizeof(s->input_pair_name), p->name, sizeof(p->name));
    s->input_pair_window = p->window;
    s->input_pair_deadline_ms = input_window_open(p) ? p->deadline_ms : 0;
    s->input_pair_stopping = p->state == KVM_DISPLAY_INPUT_RESULT && p->stopping;
    s->input_pair_busy_cause = p->state == KVM_DISPLAY_INPUT_RESULT ? p->busy_cause : 0;
    if (p->state != KVM_DISPLAY_INPUT_SCANNING || p->connecting) return;
    /* Selection sort of the used entries: strongest first, at most four. */
    bool taken[KVM_DISPLAY_INPUT_SCAN_MAX] = {false};
    for (uint8_t row = 0; row < 4u; ++row) {
        int best = -1;
        for (uint8_t i = 0; i < KVM_DISPLAY_INPUT_SCAN_MAX; ++i) {
            if (!p->scan[i].used || taken[i]) continue;
            if (best < 0 || scan_before(&p->scan[i], &p->scan[best])) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        const kvm_display_input_scan_t *entry = &p->scan[best];
        const char *label = entry->name[0] ? entry->name :
                            entry->kind == KVM_DISPLAY_INPUT_KEYBOARD ? "Keyboard" :
                            entry->kind == KVM_DISPLAY_INPUT_MOUSE ? "Mouse" : "Input device";
        copy_name(s->input_pair_rows[row], sizeof(s->input_pair_rows[row]), label,
                  entry->name[0] ? sizeof(entry->name) : strlen(label) + 1u);
        s->input_pair_index[row] = entry->index;
        s->input_pair_count = (uint8_t)(row + 1u);
    }
}
