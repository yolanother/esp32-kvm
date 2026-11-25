/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks confirmed device screen priority, guest row eligibility, numeric
 * pairing expiry, and deterministic PLUS/BOOT button behavior. */
#include "display_model.h"
#include "display_pairing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- Input-device pairing (worker state machine + view + touch) ---- */
static int begin_calls, select_calls, cancel_calls, last_select;
static bool begin_ok, select_ok, cancel_ok;
static kvm_display_input_op_t begin_fail = KVM_DISPLAY_INPUT_OP_REFUSED;
static kvm_display_input_op_t select_fail = KVM_DISPLAY_INPUT_OP_REFUSED;
static kvm_display_input_op_t fake_begin(void *c)
{ (void)c; begin_calls++; return begin_ok ? KVM_DISPLAY_INPUT_OP_OK : begin_fail; }
static kvm_display_input_op_t fake_select(void *c, uint8_t index)
{ (void)c; select_calls++; last_select = index; return select_ok ? KVM_DISPLAY_INPUT_OP_OK : select_fail; }
static bool fake_cancel(void *c) { (void)c; cancel_calls++; return cancel_ok; }

static void input_reset(kvm_display_input_pairing_t *p)
{
    begin_calls = select_calls = cancel_calls = 0;
    last_select = -1;
    begin_ok = select_ok = cancel_ok = true;
    begin_fail = select_fail = KVM_DISPLAY_INPUT_OP_REFUSED;
    kvm_display_input_pairing_init(p, (kvm_display_input_ops_t){fake_begin, fake_select, fake_cancel, NULL});
}
/* The worker's snapshot, the way publish_status builds it, then the view. */
static void input_view(const kvm_display_input_pairing_t *p, uint64_t now,
                       kvm_display_status_t *s, kvm_display_view_t *v)
{
    memset(s, 0, sizeof *s);
    s->touch_available = true;
    s->paired_guests = 1;
    s->guest_slots = s->ready_slots = 1; s->ready_mask = s->occupied_mask = 1;
    kvm_display_input_pairing_status(p, now, s);
    kvm_display_make_view(s, now, v);
}
static bool input_touch_request(const kvm_display_status_t *s, uint16_t x, uint16_t y,
                                kvm_display_input_request_t *r)
{
    kvm_display_input_action_t a;
    uint8_t row = 0xff;
    return kvm_display_input_touch_action(s, x, y, &a, &row) &&
           kvm_display_input_request(s, a, row, r);
}
static kvm_display_input_event_t scan_ev(uint8_t index, int8_t rssi, uint8_t kind, const char *name)
{
    kvm_display_input_event_t e;
    memset(&e, 0, sizeof e);
    e.type = KVM_DISPLAY_INPUT_EVENT_SCAN;
    e.index = index; e.rssi = rssi; e.kind = kind;
    snprintf(e.name, sizeof e.name, "%s", name);
    return e;
}
static kvm_display_input_event_t ev(kvm_display_input_event_type_t type, uint8_t kind,
                                    uint32_t passkey, uint8_t reason)
{
    kvm_display_input_event_t e;
    memset(&e, 0, sizeof e);
    e.type = type; e.kind = kind; e.passkey = passkey; e.reason = reason;
    return e;
}
static kvm_display_input_request_t begin_req(void)
{
    kvm_display_input_request_t r = {KVM_DISPLAY_INPUT_PAIR_BEGIN, 0, 0, 0};
    return r;
}

static void input_pairing_tests(void)
{
    kvm_display_input_pairing_t p;
    kvm_display_status_t s;
    kvm_display_view_t v;
    kvm_display_input_request_t r;
    kvm_display_input_action_t a;
    kvm_display_pair_action_t guest_action;
    uint8_t row;

    /* Entry: "Add keyboard or mouse" sits beside the guest settings gear,
       under the same show_guest_settings gate; the gear itself is unchanged. */
    input_reset(&p);
    input_view(&p, 1000, &s, &v);
    assert(v.screen == KVM_DISPLAY_ACTIVE && v.show_guest_settings && v.show_input_entry);
    /* The entry opens the input-device list (forget rows and "Add"). */
    assert(kvm_display_input_touch_action(&s, 160, 214, &a, &row) && a == KVM_DISPLAY_INPUT_DEVICES_OPEN);
    assert(kvm_display_pair_touch_action(&s, 210, 214, &guest_action) &&
           guest_action == KVM_DISPLAY_PAIR_BEGIN);
    assert(!kvm_display_input_touch_action(&s, 210, 214, &a, &row)); /* the gear stays guest pairing */
    assert(!kvm_display_input_touch_action(&s, 160, 150, &a, &row));
    s.paired_guests = 0; /* no guest settings, no entry */
    kvm_display_make_view(&s, 1000, &v);
    assert(!v.show_input_entry && !kvm_display_input_touch_action(&s, 160, 214, &a, &row));
    s.paired_guests = 1; s.touch_available = false;
    assert(!kvm_display_input_touch_action(&s, 160, 214, &a, &row));
    s.touch_available = true; s.pairing_state = KVM_DISPLAY_PAIRING_WAITING; s.pairing_deadline_ms = 9000;
    assert(!kvm_display_input_touch_action(&s, 160, 214, &a, &row)); /* guest window open */

    /* Refused while the PC drives a guest (router armed) or a guest window is
       open: BUSY with the cause, central untouched. */
    input_reset(&p);
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, true, false));
    assert(begin_calls == 0 && p.state == KVM_DISPLAY_INPUT_RESULT &&
           p.reason == KVM_DISPLAY_INPUT_REASON_BUSY);
    input_view(&p, 1000, &s, &v);
    assert(v.screen == KVM_DISPLAY_INPUT_PAIR && strcmp(v.primary, "BUSY") == 0);
    assert(strcmp(v.detail, "PC is controlling a guest") == 0);
    input_reset(&p);
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, false, true));
    assert(begin_calls == 0 && p.reason == KVM_DISPLAY_INPUT_REASON_BUSY);
    input_view(&p, 1000, &s, &v);
    assert(strcmp(v.detail, "Close guest pairing first") == 0);
    /* The result dismisses itself back to the guest view. */
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_RESULT_MS - 1, false);
    assert(p.state == KVM_DISPLAY_INPUT_RESULT);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_RESULT_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED);
    input_view(&p, 6000, &s, &v);
    assert(v.screen == KVM_DISPLAY_ACTIVE && s.input_pair_state == 0);

    /* Scanning view. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    assert(begin_calls == 1 && p.state == KVM_DISPLAY_INPUT_SCANNING);
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1100, false, false)); /* one window */
    assert(begin_calls == 1);
    assert(kvm_display_input_pairing_blocks_guest(&p));
    input_view(&p, 1000, &s, &v);
    assert(s.input_pair_state == 1 && v.screen == KVM_DISPLAY_INPUT_PAIR);
    assert(strcmp(v.title, "ADD KEYBOARD/MOUSE") == 0);
    assert(strcmp(v.primary, "SEARCHING") == 0 && v.input_row_count == 0);
    assert(strcmp(v.footer, "30s remaining") == 0);
    assert(!v.show_input_entry && !v.show_guest_settings && v.input_cancel);
    /* Guest pairing zones are dead while the input window is open. */
    assert(!kvm_display_pair_touch_action(&s, 210, 214, &guest_action));
    assert(!kvm_display_pair_touch_action(&s, 120, 180, &guest_action));
    {
        kvm_display_pair_request_t gr;
        assert(!kvm_display_pair_request(&s, 1000, KVM_DISPLAY_PAIR_BEGIN, &gr));
    }

    /* Rows arrive unsorted, more than four, one index updated: strongest four shown. */
    {
        kvm_display_input_event_t e;
        e = scan_ev(0, -80, KVM_DISPLAY_INPUT_MOUSE, "Mouse A");
        kvm_display_input_pairing_event(&p, &e, 1100);
        e = scan_ev(1, -40, KVM_DISPLAY_INPUT_KEYBOARD, "Keyboard K");
        kvm_display_input_pairing_event(&p, &e, 1100);
        e = scan_ev(2, -60, KVM_DISPLAY_INPUT_MOUSE, "");
        kvm_display_input_pairing_event(&p, &e, 1100);
        e = scan_ev(3, -90, KVM_DISPLAY_INPUT_KEYBOARD, "Far");
        kvm_display_input_pairing_event(&p, &e, 1100);
        e = scan_ev(4, -50, 0, "Mid");
        kvm_display_input_pairing_event(&p, &e, 1100);
        e = scan_ev(0, -30, KVM_DISPLAY_INPUT_MOUSE, "Mouse A"); /* repeated index updates */
        kvm_display_input_pairing_event(&p, &e, 1200);
        e = scan_ev(200, -10, 0, "Bogus"); /* out of range index is ignored */
        kvm_display_input_pairing_event(&p, &e, 1200);
    }
    input_view(&p, 2000, &s, &v);
    assert(s.input_pair_count == 4 && v.input_row_count == 4);
    assert(strcmp(s.input_pair_rows[0], "Mouse A") == 0 && s.input_pair_index[0] == 0);
    assert(strcmp(s.input_pair_rows[1], "Keyboard K") == 0 && s.input_pair_index[1] == 1);
    assert(strcmp(s.input_pair_rows[2], "Mid") == 0 && s.input_pair_index[2] == 4);
    assert(strcmp(s.input_pair_rows[3], "Mouse") == 0 && s.input_pair_index[3] == 2);
    assert(strcmp(v.input_rows[3], "Mouse") == 0 && v.primary[0] == '\0');
    assert(strcmp(v.footer, "29s remaining") == 0);
    /* Touch rows are 44 px tall from y 44; each maps to its displayed row. */
    for (uint8_t i = 0; i < 4; ++i) {
        assert(kvm_display_input_touch_action(&s, 120, (uint16_t)(44 + 44 * i), &a, &row));
        assert(a == KVM_DISPLAY_INPUT_PAIR_SELECT && row == i);
        assert(kvm_display_input_touch_action(&s, 20, (uint16_t)(87 + 44 * i), &a, &row) && row == i);
    }
    assert(!kvm_display_input_touch_action(&s, 120, 132 + 100, &a, &row));
    assert(kvm_display_input_touch_action(&s, 210, 20, &a, &row) && a == KVM_DISPLAY_INPUT_PAIR_CANCEL);
    /* Displayed row 2 is the central's stable scan index 4. */
    assert(input_touch_request(&s, 120, 140, &r));
    assert(r.action == KVM_DISPLAY_INPUT_PAIR_SELECT && r.row == 2 && r.scan_index == 4);
    assert(r.window == p.window && r.window != 0);
    /* A request from an older window is ignored. */
    {
        kvm_display_input_request_t stale = r;
        stale.window = r.window - 1;
        assert(!kvm_display_input_pairing_request(&p, stale, 3000, false, false));
        assert(select_calls == 0);
    }
    assert(kvm_display_input_pairing_request(&p, r, 3000, false, false));
    assert(select_calls == 1 && last_select == 4 && p.connecting);
    input_view(&p, 3000, &s, &v);
    assert(strcmp(v.primary, "CONNECTING") == 0 && strcmp(v.detail, "Mid") == 0);
    assert(v.input_row_count == 0 && strcmp(v.footer, "30s remaining") == 0); /* deadline re-armed at select */
    assert(!kvm_display_input_touch_action(&s, 120, 64, &a, &row)); /* no rows while connecting */
    /* Scan rows after the select are ignored. */
    {
        kvm_display_input_event_t e = scan_ev(5, -20, 0, "Late");
        kvm_display_input_pairing_event(&p, &e, 3100);
        input_view(&p, 3100, &s, &v);
        assert(s.input_pair_count == 0);
    }

    /* Passkey: TYPE ON KEYBOARD and six zero-padded digits. */
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_PASSKEY, KVM_DISPLAY_INPUT_KEYBOARD, 1234, 0);
        kvm_display_input_pairing_event(&p, &e, 4000);
    }
    assert(p.state == KVM_DISPLAY_INPUT_PASSKEY);
    input_view(&p, 4000, &s, &v);
    assert(s.input_pair_state == 2 && s.input_pair_passkey == 1234);
    assert(strcmp(v.title, "TYPE ON KEYBOARD") == 0 && strcmp(v.primary, "001234") == 0);
    assert(strcmp(v.detail, "Then press Enter") == 0);
    assert(kvm_display_input_touch_action(&s, 210, 20, &a, &row) && a == KVM_DISPLAY_INPUT_PAIR_CANCEL);

    /* Paired, then auto-dismissed. */
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_PAIRED, KVM_DISPLAY_INPUT_KEYBOARD, 0, 0);
        snprintf(e.name, sizeof e.name, "%s", "Keyboard K");
        kvm_display_input_pairing_event(&p, &e, 5000);
    }
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.reason == 0);
    assert(kvm_display_input_pairing_blocks_guest(&p)); /* until the result is dismissed */
    input_view(&p, 5000, &s, &v);
    assert(s.input_pair_state == 3 && s.input_pair_reason == 0);
    /* The result names where input now goes: here This PC (the stub), with the chord hint. */
    assert(strcmp(v.primary, "KEYBOARD PAIRED") == 0 && strcmp(v.detail, "Input goes to This PC") == 0);
    assert(strcmp(v.footer, "This PC: use Ctrl+Alt+1-3") == 0);
    /* On a guest (the default once one is ready): its card label, and the device name. */
    s.hub_target = 1;
    kvm_display_make_view(&s, 5000, &v);
    assert(strcmp(v.detail, "Input goes to Guest 1") == 0 && strcmp(v.footer, "Keyboard K") == 0);
    snprintf(s.guest_names[0], sizeof s.guest_names[0], "%s", "Desk PC");
    kvm_display_make_view(&s, 5000, &v);
    assert(strcmp(v.detail, "Input goes to Desk PC") == 0);
    s.hub_target = 3; /* not shown on screen: named by number */
    kvm_display_make_view(&s, 5000, &v);
    assert(strcmp(v.detail, "Input goes to Guest 3") == 0);
    /* A failure after the result does not overwrite it. */
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, 2);
        kvm_display_input_pairing_event(&p, &e, 5100);
        assert(p.reason == 0);
    }
    /* Touch dismisses the result at once. */
    input_view(&p, 5200, &s, &v);
    assert(input_touch_request(&s, 210, 20, &r) && r.action == KVM_DISPLAY_INPUT_PAIR_CANCEL);
    assert(kvm_display_input_pairing_request(&p, r, 5200, false, false));
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED && cancel_calls == 0); /* nothing to cancel */

    /* Failure reasons 1..5 carry their text. */
    {
        static const char *const primary[6] = {"", "NOT SUPPORTED", "TIMED OUT",
                                               "PAIRING FAILED", "BUSY", "NO FREE SLOT"};
        for (uint8_t reason = 1; reason <= 5; ++reason) {
            input_reset(&p);
            assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
            kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, reason);
            kvm_display_input_pairing_event(&p, &e, 2000);
            assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.reason == reason);
            input_view(&p, 2000, &s, &v);
            assert(strcmp(v.primary, primary[reason]) == 0);
        }
        input_view(&p, 2000, &s, &v);
        assert(strcmp(v.detail, "Forget a keyboard or mouse") == 0);
        input_reset(&p);
        assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, 1);
        kvm_display_input_pairing_event(&p, &e, 2000);
        input_view(&p, 2000, &s, &v);
        assert(strcmp(v.detail, "Device is not supported") == 0);
    }

    /* Hard 30 s deadline without any terminal event: the UI closes the window
       itself and cancels the central. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS - 1, false);
    assert(p.state == KVM_DISPLAY_INPUT_SCANNING && cancel_calls == 0);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS, false);
    assert(cancel_calls == 1 && p.state == KVM_DISPLAY_INPUT_RESULT &&
           p.reason == KVM_DISPLAY_INPUT_REASON_TIMEOUT);
    /* A late PAIRED (the device made it just in time) is the truth and replaces it. */
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_PAIRED, KVM_DISPLAY_INPUT_MOUSE, 0, 0);
        kvm_display_input_pairing_event(&p, &e, 31500);
        assert(p.reason == 0 && p.kind == KVM_DISPLAY_INPUT_MOUSE);
        input_view(&p, 31500, &s, &v);
        assert(strcmp(v.primary, "MOUSE PAIRED") == 0);
    }
    /* Passkey stage keeps the same hard deadline from the select. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    {
        kvm_display_input_event_t e = scan_ev(3, -50, KVM_DISPLAY_INPUT_KEYBOARD, "K");
        kvm_display_input_pairing_event(&p, &e, 1000);
        input_view(&p, 2000, &s, &v);
        assert(kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 0, &r) && r.scan_index == 3);
        assert(!kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 1, &r)); /* no such row */
        assert(kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 0, &r));
        assert(kvm_display_input_pairing_request(&p, r, 20000, false, false));
        e = ev(KVM_DISPLAY_INPUT_EVENT_PASSKEY, KVM_DISPLAY_INPUT_KEYBOARD, 999999, 0);
        kvm_display_input_pairing_event(&p, &e, 21000);
        input_view(&p, 21000, &s, &v);
        assert(strcmp(v.primary, "999999") == 0);
        kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS, false);
        assert(p.state == KVM_DISPLAY_INPUT_PASSKEY); /* not the begin deadline */
        kvm_display_input_pairing_tick(&p, 20000 + KVM_DISPLAY_INPUT_WINDOW_MS, false);
        assert(p.state == KVM_DISPLAY_INPUT_RESULT && cancel_calls == 1 &&
               p.reason == KVM_DISPLAY_INPUT_REASON_TIMEOUT);
    }
    /* A malformed passkey is never shown. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_PASSKEY, 1, 1000000, 0);
        kvm_display_input_pairing_event(&p, &e, 1500);
        assert(p.state != KVM_DISPLAY_INPUT_PASSKEY);
    }

    /* An open, unarmed host session is fine (tick's flag is router.armed);
       the router arming mid-window ends the window: the host route wins. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    kvm_display_input_pairing_tick(&p, 2000, false);
    assert(cancel_calls == 0 && p.state == KVM_DISPLAY_INPUT_SCANNING);
    kvm_display_input_pairing_tick(&p, 2100, true);
    /* No cancel RPC from the worker while the PC is armed (it would stall host
       input): the window is shown BUSY and stays "stopping", which keeps the
       guest gate closed, until the central's own timer has ended it. */
    assert(cancel_calls == 0 && p.state == KVM_DISPLAY_INPUT_RESULT &&
           p.reason == KVM_DISPLAY_INPUT_REASON_BUSY && p.stopping);
    assert(kvm_display_input_pairing_blocks_guest(&p));
    input_view(&p, 2100, &s, &v);
    assert(strcmp(v.detail, "PC is controlling a guest") == 0);
    for (uint64_t t = 2200; t < 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS;
         t += 500)
        kvm_display_input_pairing_tick(&p, t, true);
    assert(cancel_calls == 0 && p.stopping && kvm_display_input_pairing_blocks_guest(&p));
    /* Even the owner's cancel tap does not call the central while armed. */
    {
        kvm_display_input_request_t c = {KVM_DISPLAY_INPUT_PAIR_CANCEL, 0, 0, p.window};
        assert(!kvm_display_input_pairing_request(&p, c, 20000, true, false));
        assert(cancel_calls == 0 && p.stopping);
    }
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS,
                                   true);
    assert(cancel_calls == 0 && p.state == KVM_DISPLAY_INPUT_CLOSED);
    /* The route disarms before the deadline: the deferred cancel runs then, once. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    kvm_display_input_pairing_tick(&p, 2000, true);
    assert(cancel_calls == 0 && p.stopping);
    kvm_display_input_pairing_tick(&p, 2100, false);
    assert(cancel_calls == 1 && !p.stopping);
    kvm_display_input_pairing_tick(&p, 2000 + KVM_DISPLAY_INPUT_RESULT_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED && cancel_calls == 1);
    /* A row tapped while the PC is armed is refused with BUSY (no select RPC). */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    {
        kvm_display_input_event_t e = scan_ev(2, -40, KVM_DISPLAY_INPUT_KEYBOARD, "K");
        kvm_display_input_request_t sel = {KVM_DISPLAY_INPUT_PAIR_SELECT, 0, 2, p.window};
        int before = select_calls;
        kvm_display_input_pairing_event(&p, &e, 1100);
        assert(!kvm_display_input_pairing_request(&p, sel, 1200, true, false));
        assert(select_calls == before && cancel_calls == 0);
        assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.reason == KVM_DISPLAY_INPUT_REASON_BUSY &&
               p.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST && p.stopping);
    }

    /* A begin whose RPC timed out may still open the window later: BUSY is
       shown, but the guest gate stays closed, cancel is retried (bounded rate)
       and the window is only reported closed once the central confirms. */
    input_reset(&p);
    begin_ok = false; begin_fail = KVM_DISPLAY_INPUT_OP_UNKNOWN; cancel_ok = false;
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    assert(begin_calls == 1 && cancel_calls == 1 && p.stopping);
    input_view(&p, 1000, &s, &v);
    assert(s.input_pair_state == KVM_DISPLAY_INPUT_RESULT && s.input_pair_stopping);
    assert(strcmp(v.primary, "BUSY") == 0 && strcmp(v.footer, "Stopping pairing...") == 0);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_RESULT_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && kvm_display_input_pairing_blocks_guest(&p));
    assert(cancel_calls == 2); /* retried at most once per retry interval */
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_RESULT_MS + 10, false);
    assert(cancel_calls == 2);
    for (uint64_t t = 6000; t < 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS;
         t += 500)
        kvm_display_input_pairing_tick(&p, t, false);
    assert(kvm_display_input_pairing_blocks_guest(&p) && cancel_calls > 10 && cancel_calls < 40);
    /* The central's own window timer has surely ended it: release. */
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS,
                                   false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED && !kvm_display_input_pairing_blocks_guest(&p));
    /* A confirmed cancel ends the hold early (it ran after the abandoned begin). */
    input_reset(&p);
    begin_ok = false; begin_fail = KVM_DISPLAY_INPUT_OP_UNKNOWN; cancel_ok = false;
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    cancel_ok = true;
    kvm_display_input_pairing_tick(&p, 1500, false); /* before the retry interval */
    assert(cancel_calls == 1 && p.stopping);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_CANCEL_RETRY_MS, false);
    assert(cancel_calls == 2 && !p.stopping && p.state == KVM_DISPLAY_INPUT_RESULT);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_RESULT_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED);
    /* A plain refusal (no timeout) needs no hold. */
    input_reset(&p);
    begin_ok = false;
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    assert(cancel_calls == 0 && !p.stopping);
    /* An owner cancel the central does not confirm is not reported closed. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    cancel_ok = false;
    input_view(&p, 1500, &s, &v);
    assert(input_touch_request(&s, 200, 10, &r) && r.action == KVM_DISPLAY_INPUT_PAIR_CANCEL);
    assert(!kvm_display_input_pairing_request(&p, r, 1500, false, false));
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.stopping &&
           kvm_display_input_pairing_blocks_guest(&p));
    input_view(&p, 1500, &s, &v);
    assert(strcmp(v.primary, "CANCELLED") == 0 && strcmp(v.footer, "Stopping pairing...") == 0);
    cancel_ok = true; /* pressing close again retries at once */
    input_view(&p, 1600, &s, &v);
    assert(input_touch_request(&s, 200, 10, &r));
    assert(kvm_display_input_pairing_request(&p, r, 1600, false, false));
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED && cancel_calls == 2);
    /* A timed-out select is held the same way. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    {
        kvm_display_input_event_t e = scan_ev(2, -50, 1, "K");
        kvm_display_input_pairing_event(&p, &e, 1000);
        input_view(&p, 1100, &s, &v);
        select_ok = false; select_fail = KVM_DISPLAY_INPUT_OP_UNKNOWN; cancel_ok = false;
        assert(kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 0, &r));
        assert(!kvm_display_input_pairing_request(&p, r, 1100, false, false));
        assert(p.stopping && p.stop_deadline_ms ==
               1100 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS);
    }
    /* The hard deadline with an unconfirmed cancel holds too. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    cancel_ok = false;
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS, false);
    assert(p.reason == KVM_DISPLAY_INPUT_REASON_TIMEOUT && p.stopping);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_RESULT_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_RESULT);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_WINDOW_MS + KVM_DISPLAY_INPUT_STOP_MARGIN_MS,
                                   false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED);

    /* Cancel at any time: scanning, connecting and passkey all cancel the central. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    input_view(&p, 1500, &s, &v);
    assert(input_touch_request(&s, 200, 10, &r) && r.action == KVM_DISPLAY_INPUT_PAIR_CANCEL);
    assert(kvm_display_input_pairing_request(&p, r, 1500, false, false));
    assert(cancel_calls == 1 && p.state == KVM_DISPLAY_INPUT_CLOSED);
    assert(!kvm_display_input_pairing_blocks_guest(&p));
    /* Events with the window closed are ignored. */
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, 3);
        kvm_display_input_pairing_event(&p, &e, 1600);
        e = scan_ev(0, -40, 1, "X");
        kvm_display_input_pairing_event(&p, &e, 1600);
        assert(p.state == KVM_DISPLAY_INPUT_CLOSED);
    }

    /* The central refuses begin: BUSY at once, refined by its PAIR_FAILED reason. */
    input_reset(&p);
    begin_ok = false;
    assert(!kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    assert(begin_calls == 1 && p.state == KVM_DISPLAY_INPUT_RESULT &&
           p.reason == KVM_DISPLAY_INPUT_REASON_BUSY);
    {
        kvm_display_input_event_t e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, 5);
        kvm_display_input_pairing_event(&p, &e, 1010);
        assert(p.reason == 5);
        e.reason = 2; /* only the first reason refines */
        kvm_display_input_pairing_event(&p, &e, 1020);
        assert(p.reason == 5);
    }
    /* A select the central refuses ends the window too. */
    input_reset(&p);
    select_ok = false;
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    {
        kvm_display_input_event_t e = scan_ev(1, -50, 2, "M");
        kvm_display_input_pairing_event(&p, &e, 1000);
        input_view(&p, 1100, &s, &v);
        assert(kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 0, &r));
        assert(!kvm_display_input_pairing_request(&p, r, 1100, false, false));
        assert(select_calls == 1 && cancel_calls == 1 && p.state == KVM_DISPLAY_INPUT_RESULT);
        e = ev(KVM_DISPLAY_INPUT_EVENT_FAILED, 0, 0, 1);
        kvm_display_input_pairing_event(&p, &e, 1110);
        assert(p.reason == 1);
    }

    /* A guest pairing request refused for the open input window is visible. */
    input_reset(&p);
    assert(kvm_display_input_pairing_request(&p, begin_req(), 1000, false, false));
    kvm_display_input_pairing_guest_refused(&p, 1000);
    input_view(&p, 1000, &s, &v);
    assert(s.input_pair_guest_busy && strcmp(v.title, "GUEST PAIRING BUSY") == 0);
    input_view(&p, 1000 + KVM_DISPLAY_INPUT_GUEST_BUSY_MS, &s, &v);
    assert(!s.input_pair_guest_busy && strcmp(v.title, "ADD KEYBOARD/MOUSE") == 0);
    /* On the passkey screen the hint never replaces the typing instructions. */
    {
        kvm_display_input_event_t e = scan_ev(1, -40, KVM_DISPLAY_INPUT_KEYBOARD, "K");
        kvm_display_input_pairing_event(&p, &e, 2000);
        input_view(&p, 2000, &s, &v);
        assert(kvm_display_input_request(&s, KVM_DISPLAY_INPUT_PAIR_SELECT, 0, &r));
        assert(kvm_display_input_pairing_request(&p, r, 2000, false, false));
        e = ev(KVM_DISPLAY_INPUT_EVENT_PASSKEY, KVM_DISPLAY_INPUT_KEYBOARD, 42, 0);
        kvm_display_input_pairing_event(&p, &e, 2100);
        kvm_display_input_pairing_guest_refused(&p, 2200);
        input_view(&p, 2200, &s, &v);
        assert(strcmp(v.title, "TYPE ON KEYBOARD") == 0 && strcmp(v.primary, "000042") == 0);
        assert(strcmp(v.detail, "Then press Enter") == 0 && strcmp(v.footer, "GUEST PAIRING BUSY") == 0);
        input_view(&p, 2200 + KVM_DISPLAY_INPUT_GUEST_BUSY_MS, &s, &v);
        assert(strcmp(v.title, "TYPE ON KEYBOARD") == 0 && strcmp(v.footer, "27s remaining") == 0);
    }

    /* Recovery and update keep precedence over the input screen. */
    input_view(&p, 1000, &s, &v);
    s.recovery_required = true;
    kvm_display_make_view(&s, 1000, &v);
    assert(v.screen == KVM_DISPLAY_RECOVERY);
    assert(!kvm_display_input_touch_action(&s, 210, 20, &a, &row));
    s.recovery_required = false; s.updating = true;
    kvm_display_make_view(&s, 1000, &v);
    assert(v.screen == KVM_DISPLAY_UPDATE);

    /* NULL safety. */
    kvm_display_input_pairing_status(NULL, 0, &s);
    kvm_display_input_pairing_status(&p, 0, NULL);
    kvm_display_input_pairing_event(NULL, NULL, 0);
    kvm_display_input_pairing_event(&p, NULL, 0);
    kvm_display_input_pairing_tick(NULL, 0, false);
    assert(!kvm_display_input_pairing_request(NULL, begin_req(), 0, false, false));
    assert(!kvm_display_input_touch_action(NULL, 0, 0, &a, &row));
    assert(!kvm_display_input_request(NULL, KVM_DISPLAY_INPUT_PAIR_CANCEL, 0, &r));
    assert(!kvm_display_input_pairing_blocks_guest(NULL));
}

/* ---- Input-device list: forget a keyboard or mouse with a confirm tap ---- */
static int forget_calls, last_forget;
static kvm_display_input_op_t forget_result = KVM_DISPLAY_INPUT_OP_OK;
static kvm_display_input_op_t fake_forget(void *c, uint8_t kind)
{ (void)c; forget_calls++; last_forget = kind; return forget_result; }

static void devices_reset(kvm_display_input_pairing_t *p)
{
    input_reset(p);
    forget_calls = 0;
    last_forget = -1;
    forget_result = KVM_DISPLAY_INPUT_OP_OK;
    p->ops.forget = fake_forget;
    kvm_display_input_pairing_device(p, KVM_DISPLAY_INPUT_KEYBOARD, true, "K380");
    kvm_display_input_pairing_device(p, KVM_DISPLAY_INPUT_MOUSE, false, NULL);
}
static kvm_display_input_request_t devices_open_req(void)
{
    kvm_display_input_request_t r = {KVM_DISPLAY_INPUT_DEVICES_OPEN, 0, 0, 0};
    return r;
}
static bool devices_tap(kvm_display_input_pairing_t *p, uint16_t x, uint16_t y, uint64_t now,
                        bool host_armed, bool guest_pairing)
{
    kvm_display_status_t s;
    kvm_display_view_t v;
    kvm_display_input_request_t r;
    input_view(p, now, &s, &v);
    return input_touch_request(&s, x, y, &r) &&
           kvm_display_input_pairing_request(p, r, now, host_armed, guest_pairing);
}

static void input_devices_tests(void)
{
    kvm_display_input_pairing_t p;
    kvm_display_status_t s;
    kvm_display_view_t v;
    kvm_display_input_request_t r = {0};
    kvm_display_input_action_t a = KVM_DISPLAY_INPUT_PAIR_CANCEL;
    uint8_t row = 0;

    /* The entry opens the list; nothing reaches the central. */
    devices_reset(&p);
    assert(devices_tap(&p, 160, 214, 1000, false, false));
    assert(p.state == KVM_DISPLAY_INPUT_DEVICES && begin_calls == 0 && forget_calls == 0);
    assert(kvm_display_input_pairing_blocks_guest(&p));
    input_view(&p, 1000, &s, &v);
    assert(v.screen == KVM_DISPLAY_INPUT_PAIR && strcmp(v.title, "INPUT DEVICES") == 0);
    assert(v.input_row_count == 3 && v.input_cancel);
    assert(strcmp(v.input_rows[0], "KEYBOARD: K380") == 0);
    assert(strcmp(v.input_rows[1], "MOUSE: not paired") == 0);
    assert(strcmp(v.input_rows[2], "ADD KEYBOARD OR MOUSE") == 0);
    assert(!v.show_input_entry && !v.show_guest_settings);

    /* First tap on a paired row asks for confirmation; nothing is forgotten. */
    assert(devices_tap(&p, 120, 64, 1100, false, false));
    assert(forget_calls == 0);
    input_view(&p, 1100, &s, &v);
    assert(strcmp(v.title, "FORGET K380?") == 0);
    assert(strcmp(v.input_rows[0], "TAP AGAIN TO FORGET") == 0);
    assert(strcmp(v.footer, "Tap again within 5s") == 0);
    /* The second tap within 5 s forgets that kind. */
    assert(devices_tap(&p, 120, 64, 1100 + KVM_DISPLAY_INPUT_FORGET_CONFIRM_MS - 1, false, false));
    assert(forget_calls == 1 && last_forget == KVM_DISPLAY_INPUT_KEYBOARD);
    input_view(&p, 6200, &s, &v);
    assert(p.state == KVM_DISPLAY_INPUT_DEVICES && strcmp(v.title, "INPUT DEVICES") == 0);
    assert(strcmp(v.input_rows[0], "KEYBOARD: not paired") == 0);
    assert(strcmp(v.footer, "KEYBOARD FORGOTTEN") == 0);
    /* An unpaired row does nothing. */
    assert(!devices_tap(&p, 120, 64, 6300, false, false) && forget_calls == 1);
    assert(!devices_tap(&p, 120, 108, 6300, false, false) && forget_calls == 1);

    /* A second tap after 5 s only asks again. */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 64, 1000, false, false));
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_FORGET_CONFIRM_MS, false);
    input_view(&p, 1000 + KVM_DISPLAY_INPUT_FORGET_CONFIRM_MS, &s, &v);
    assert(strcmp(v.title, "INPUT DEVICES") == 0 && strcmp(v.input_rows[0], "KEYBOARD: K380") == 0);
    assert(devices_tap(&p, 120, 64, 1000 + KVM_DISPLAY_INPUT_FORGET_CONFIRM_MS, false, false));
    assert(forget_calls == 0);
    /* A confirm for one kind is not a confirm for the other. */
    kvm_display_input_pairing_device(&p, KVM_DISPLAY_INPUT_MOUSE, true, "MX");
    assert(devices_tap(&p, 120, 108, 7000, false, false) && forget_calls == 0);
    assert(devices_tap(&p, 120, 108, 7100, false, false) && forget_calls == 1 &&
           last_forget == KVM_DISPLAY_INPUT_MOUSE);

    /* A refused forget says so and keeps the device listed. */
    devices_reset(&p);
    forget_result = KVM_DISPLAY_INPUT_OP_REFUSED;
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 64, 1000, false, false));
    assert(!devices_tap(&p, 120, 64, 1200, false, false) && forget_calls == 1);
    input_view(&p, 1200, &s, &v);
    assert(strcmp(v.footer, "FORGET FAILED, RETRY") == 0 && strcmp(v.input_rows[0], "KEYBOARD: K380") == 0);

    /* A forget whose request timed out may still run: FORGETTING..., then the
       worker's next refresh of the stored devices settles it. */
    devices_reset(&p);
    forget_result = KVM_DISPLAY_INPUT_OP_UNKNOWN;
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 64, 1000, false, false));
    assert(!devices_tap(&p, 120, 64, 1200, false, false) && forget_calls == 1);
    input_view(&p, 1200, &s, &v);
    assert(strcmp(v.footer, "FORGETTING...") == 0 && strcmp(v.input_rows[0], "KEYBOARD: K380") == 0);
    kvm_display_input_pairing_device(&p, KVM_DISPLAY_INPUT_KEYBOARD, true, "K380"); /* not yet */
    input_view(&p, 1300, &s, &v);
    assert(strcmp(v.footer, "FORGETTING...") == 0);
    kvm_display_input_pairing_device(&p, KVM_DISPLAY_INPUT_KEYBOARD, false, NULL); /* it ran */
    input_view(&p, 1400, &s, &v);
    assert(strcmp(v.footer, "KEYBOARD FORGOTTEN") == 0 && strcmp(v.input_rows[0], "KEYBOARD: not paired") == 0);

    /* "Add keyboard or mouse" starts the pairing window from the list. */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 152, 1100, false, false));
    assert(begin_calls == 1 && p.state == KVM_DISPLAY_INPUT_SCANNING);
    assert(p.stored_mask == (1u << KVM_DISPLAY_INPUT_KEYBOARD)); /* kept across the window */

    /* The close control leaves the list without touching the central. */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 210, 20, 1100, false, false));
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED && cancel_calls == 0);

    /* Gating: never while the PC drives a guest or a guest window is open. */
    devices_reset(&p);
    assert(!kvm_display_input_pairing_request(&p, devices_open_req(), 1000, true, false));
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST);
    devices_reset(&p);
    assert(!kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, true));
    assert(p.busy_cause == KVM_DISPLAY_INPUT_BUSY_GUEST);
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 64, 1000, false, false));
    assert(!devices_tap(&p, 120, 64, 1100, true, false) && forget_calls == 0);
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && p.busy_cause == KVM_DISPLAY_INPUT_BUSY_HOST);
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    assert(devices_tap(&p, 120, 64, 1000, false, false));
    assert(!devices_tap(&p, 120, 64, 1100, false, true) && forget_calls == 0);
    /* The host arming while the list shows closes it (no central call). */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    kvm_display_input_pairing_tick(&p, 1100, true);
    assert(p.state == KVM_DISPLAY_INPUT_RESULT && cancel_calls == 0 && forget_calls == 0);
    /* The model refuses the touches too while armed or beside a guest window. */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    input_view(&p, 1000, &s, &v);
    s.armed = true; s.guest_ready = true;
    assert(!(kvm_display_input_touch_action(&s, 120, 64, &a, &row) &&
             kvm_display_input_request(&s, a, row, &r)));
    s.armed = false; s.pairing_state = KVM_DISPLAY_PAIRING_WAITING; s.pairing_deadline_ms = 9000;
    assert(!(kvm_display_input_touch_action(&s, 120, 64, &a, &row) &&
             kvm_display_input_request(&s, a, row, &r)));
    /* A stale window's tap is ignored. */
    s.pairing_state = 0;
    assert(kvm_display_input_touch_action(&s, 120, 64, &a, &row) &&
           kvm_display_input_request(&s, a, row, &r) && a == KVM_DISPLAY_INPUT_FORGET);
    r.window++;
    assert(!kvm_display_input_pairing_request(&p, r, 1100, false, false));

    /* An idle list closes itself. */
    devices_reset(&p);
    assert(kvm_display_input_pairing_request(&p, devices_open_req(), 1000, false, false));
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_DEVICES_IDLE_MS - 1, false);
    assert(p.state == KVM_DISPLAY_INPUT_DEVICES);
    kvm_display_input_pairing_tick(&p, 1000 + KVM_DISPLAY_INPUT_DEVICES_IDLE_MS, false);
    assert(p.state == KVM_DISPLAY_INPUT_CLOSED);
    /* Stored names survive the close and project into every snapshot. */
    input_view(&p, 40000, &s, &v);
    assert(s.input_dev_stored == (1u << KVM_DISPLAY_INPUT_KEYBOARD) &&
           strcmp(s.input_dev_names[0], "K380") == 0);
}

int main(void)
{
    kvm_display_status_t status = {0};
    assert(strcmp(kvm_display_target_label(&status), "HOST") == 0);
    status.selected_slot = 1;
    assert(strcmp(kvm_display_target_label(&status), "DISARMED") == 0);
    status.armed = true;
    assert(strcmp(kvm_display_target_label(&status), "LOCAL ERROR") == 0);
    status.guest_ready = true;
    assert(strcmp(kvm_display_target_label(&status), "GUEST 1") == 0);
    status.fault = true;
    assert(strcmp(kvm_display_target_label(&status), "LOCAL ERROR") == 0);

    kvm_display_view_t view;
    status.fault = false;
    status.usb_connected = true;
    status.guest_slots = 2;
    status.ready_slots = 1;
    status.ready_mask = 1;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE);
    assert(strcmp(view.primary, "GUEST 1") == 0);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    status.guest_ready = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAUSED);
    assert(strcmp(view.primary, "INPUT PAUSED") == 0);
    assert(strcmp(view.title, "ROUTING FAULT") == 0);
    assert(strcmp(view.footer, "Use host to release") == 0);
    status.guest_ready = true;
    status.armed = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "DISARMED") == 0);
    /* usb_off_with_ready_guest_shows_active_dimmed */
    status.usb_connected = false;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE);
    assert(view.usb_dim);
    assert(strcmp(view.title, "KEYBOARD CONTROL") == 0);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    assert(strcmp(view.footer, "USB off") == 0);
    assert(strcmp(view.primary, "DISARMED") == 0);
    status.usb_connected = true;
    status.show_guest_list = true;
    status.touch_available = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_GUEST_LIST);
    assert(strcmp(view.rows[0], "GUEST 1  STANDBY") == 0);
    assert(strcmp(view.rows[1], "GUEST 2  OFFLINE") == 0);
    assert(kvm_display_touch_slot(&view, 64) == 1);
    assert(kvm_display_touch_slot(&view, 110) == 0);
    assert(kvm_display_touch_slot(&view, 240) == 0);
    status.ready_mask = 2;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.rows[0], "GUEST 1  OFFLINE") == 0);
    assert(strcmp(view.rows[1], "GUEST 2  STANDBY") == 0);
    assert(kvm_display_touch_slot(&view, 64) == 0);
    assert(kvm_display_touch_slot(&view, 110) == 2);
    assert(kvm_display_next_ready_slot(0b110, 0) == 2);
    assert(kvm_display_next_ready_slot(0b110, 2) == 3);
    assert(kvm_display_next_ready_slot(0b110, 3) == 0);
    status.show_guest_list = false;
    status.pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
    status.pairing_local_owner = true;
    status.pairing_challenge_id = 7;
    status.pairing_number = 123;
    status.pairing_deadline_ms = 61000;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "000123") == 0);
    assert(strcmp(view.detail, "60s remaining") == 0);
    status.ready_slots = 0;
    kvm_display_make_view(&status, 61000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.ready_slots = 1;
    status.usb_connected = false;
    kvm_display_make_view(&status, 61000, &view);
    assert(view.screen == KVM_DISPLAY_ACTIVE && view.usb_dim);
    assert(strcmp(view.detail, "1 guest connected") == 0);
    status.usb_connected = true;
    status.ready_slots = 1;
    status.pairing_challenge_id = 0;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_challenge_id = 7;
    status.pairing_number = 1000000;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.ready_slots = 0;
    status.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
    status.pairing_deadline_ms = 6100;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "WAITING FOR GUEST") == 0);
    assert(strcmp(view.detail, "6s remaining") == 0);
    kvm_display_make_view(&status, 6100, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.pairing_deadline_ms = 0;
    kvm_display_make_view(&status, 1000, &view);
    assert(strcmp(view.primary, "PAIRING EXPIRED") == 0);
    status.usb_connected = false;
    status.pairing_state = KVM_DISPLAY_PAIRING_CHALLENGE;
    status.pairing_challenge_id = 91;
    status.pairing_number = 123456;
    status.pairing_deadline_ms = 61000;
    status.touch_available = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_PAIRING);
    assert(strcmp(view.primary, "123456") == 0);
    kvm_display_pair_request_t request;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 91);
    kvm_display_pair_action_t touched;
    assert(strcmp(view.footer, "Confirm matching code on guest") == 0);
    assert(!kvm_display_pair_touch_action(&status, 40, 180, &touched));
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    assert(!kvm_display_pair_touch_action(&status, 180, 120, &touched));
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_REJECT, &request));
    assert(request.challenge_id == 91);
    status.pairing_local_owner = false;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 91);
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    status.pairing_local_owner = true;
    assert(!kvm_display_pair_request(&status, 61000, KVM_DISPLAY_PAIR_APPROVE, &request));
    assert(request.challenge_id == 0);
    status.touch_available = false;
    assert(!kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_APPROVE, &request));
    status.touch_available = true;
    status.pairing_state = KVM_DISPLAY_PAIRING_CLOSED;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_BEGIN, &request));
    assert(request.challenge_id == 0);
    assert(kvm_display_pair_touch_action(&status, 120, 180, &touched));
    assert(touched == KVM_DISPLAY_PAIR_BEGIN);
    status.paired_guests = 2;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.show_guest_settings);
    assert(!kvm_display_pair_touch_action(&status, 120, 180, &touched));
    assert(kvm_display_pair_touch_action(&status, 210, 214, &touched));
    assert(touched == KVM_DISPLAY_PAIR_BEGIN);
    status.paired_guests = 0;
    status.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
    assert(kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_CANCEL, &request));
    assert(!kvm_display_pair_touch_action(&status, 180, 180, &touched));
    assert(kvm_display_pair_touch_action(&status, 40, 180, &touched));
    assert(touched == KVM_DISPLAY_PAIR_CANCEL);
    status.pairing_state = KVM_DISPLAY_PAIRING_CAPACITY;
    assert(!kvm_display_pair_request(&status, 1000, KVM_DISPLAY_PAIR_BEGIN, &request));
    status.pairing_state = KVM_DISPLAY_PAIRING_CLOSED;
    status.usb_connected = true;
    status.recovery_required = true;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_RECOVERY);
    status.recovery_required = false;
    status.updating = true;
    status.update_percent = 42;
    kvm_display_make_view(&status, 1000, &view);
    assert(view.screen == KVM_DISPLAY_UPDATE);
    assert(strcmp(view.primary, "42%") == 0);

    /* No host session: the guest view stays, only usb_dim/footer change. */
    {
        kvm_display_status_t s = {0};
        kvm_display_view_t on, off;
        /* Toggling the session never changes the screen except usb_dim and
           footer, for every guest count, hub target, selected slot and
           keyboard state. The This PC hint (keyboard connected, input on This
           PC, nothing armed) is the same footer in both states. */
        for (uint8_t ready = 0; ready <= 3; ++ready)
        for (uint8_t hub = 0; hub <= 3; ++hub)
        for (uint8_t sel = 0; sel <= 3; ++sel)
        for (uint8_t kbd = 0; kbd <= 1; ++kbd) {
            bool hint = kbd && hub == 0;
            memset(&s, 0, sizeof s);
            s.guest_slots = ready; s.ready_slots = ready;
            s.ready_mask = s.occupied_mask = (uint8_t)((1u << ready) - 1u);
            s.guest_ready = true; s.selected_slot = sel; s.hub_target = hub;
            s.input_kbd = kbd != 0;
            s.usb_connected = s.host_session = true;
            kvm_display_make_view(&s, 5000, &on);
            s.usb_connected = s.host_session = false;
            kvm_display_make_view(&s, 5000, &off);
            assert(on.screen == KVM_DISPLAY_ACTIVE && off.screen == on.screen);
            assert(strcmp(on.title, "KEYBOARD CONTROL") == 0);
            assert(strcmp(off.title, on.title) == 0);
            assert(strcmp(off.primary, on.primary) == 0);
            assert(strcmp(off.detail, on.detail) == 0);
            assert(off.card_visible_mask == on.card_visible_mask);
            assert(off.card_ready_mask == on.card_ready_mask);
            assert(off.selected_card == on.selected_card);
            assert(on.selected_card == (hub <= ready ? hub : 0)); /* unoccupied: This PC */
            assert(!on.usb_dim && off.usb_dim);
            assert(on.footer_hint == hint && off.footer_hint == hint);
            if (hint) {
                assert(strcmp(on.footer, "This PC: use Ctrl+Alt+1-3") == 0);
                assert(strcmp(off.footer, on.footer) == 0);
            } else {
                assert(strcmp(off.footer, "USB off") == 0);
                assert(strcmp(on.footer, "Selected target is highlighted") == 0);
            }
            /* Everything else is identical once usb_dim and footer are aligned. */
            off.usb_dim = false;
            memcpy(off.footer, on.footer, sizeof off.footer);
            assert(memcmp(&on, &off, sizeof on) == 0);
        }
        /* An armed host route is not "This PC with no output": no hint. */
        memset(&s, 0, sizeof s);
        s.usb_connected = s.host_session = true; s.armed = true; s.guest_ready = true;
        s.guest_slots = s.ready_slots = 1; s.ready_mask = s.occupied_mask = 1;
        s.selected_slot = 1; s.input_kbd = true; s.hub_target = 0;
        kvm_display_make_view(&s, 5000, &on);
        assert(!on.footer_hint && strcmp(on.footer, "Selected target is highlighted") == 0);
        /* usb_off_no_guest_shows_guest_view_not_pause */
        memset(&s, 0, sizeof s);
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_ACTIVE && off.usb_dim);
        assert(strcmp(off.title, "KEYBOARD CONTROL") == 0);
        assert(strcmp(off.detail, "No guest connected") == 0);
        assert(strcmp(off.footer, "USB off") == 0);
        assert(off.card_visible_mask == 1u && off.selected_card == 0);
        /* An armed host route drives the highlight while the session is on. */
        s.usb_connected = s.host_session = true; s.armed = true; s.guest_ready = true;
        s.guest_slots = s.ready_slots = 2; s.ready_mask = s.occupied_mask = 3;
        s.selected_slot = 2; s.hub_target = 1;
        kvm_display_make_view(&s, 5000, &on);
        assert(on.selected_card == 2);
        /* Selected by the host but not armed: the highlight is the hub target
           (where the keyboard types); the host's slot shows as DISARMED. */
        s.armed = false;
        kvm_display_make_view(&s, 5000, &on);
        assert(on.selected_card == 1 && strcmp(on.primary, "DISARMED") == 0);
        memset(&s, 0, sizeof s);
        /* fault_still_pauses and armed-without-ready still pause with no session */
        s.fault = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAUSED && strcmp(off.primary, "INPUT PAUSED") == 0);
        assert(!off.usb_dim);
        s.fault = false; s.armed = true; s.guest_ready = false;
        s.ready_slots = 1; s.ready_mask = 1; s.occupied_mask = 1; s.guest_slots = 1;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAUSED && strcmp(off.title, "ROUTING FAULT") == 0);
        /* recovery_still_wins and updating still win */
        s.armed = false; s.recovery_required = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_RECOVERY && !off.usb_dim);
        s.recovery_required = false; s.updating = true;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_UPDATE);
        s.updating = false;
        /* pairing keeps precedence over the no-session view */
        s.pairing_state = KVM_DISPLAY_PAIRING_WAITING;
        s.pairing_deadline_ms = 9000;
        s.ready_slots = 0; s.ready_mask = 0; s.occupied_mask = 0; s.guest_slots = 0;
        kvm_display_make_view(&s, 5000, &off);
        assert(off.screen == KVM_DISPLAY_PAIRING && !off.usb_dim);
        /* show_guest_list unchanged with a host session */
        memset(&s, 0, sizeof s);
        s.usb_connected = s.host_session = true;
        s.guest_slots = 1; s.ready_slots = 1; s.ready_mask = 1; s.occupied_mask = 1;
        s.show_guest_list = true;
        kvm_display_make_view(&s, 5000, &on);
        assert(on.screen == KVM_DISPLAY_GUEST_LIST && !on.usb_dim);
        assert(strcmp(on.rows[0], "GUEST 1  STANDBY") == 0);
        assert(strcmp(on.footer, "Use host to select") == 0);
    }

    /* Footer badge text: USB state first, then connected input devices. */
    {
        kvm_display_status_t s;
        char out[32];
        memset(&s, 0, sizeof s);
        s.input_kbd = s.input_mouse = true;
        kvm_display_badge_text(&s, out, sizeof out);
        assert(strcmp(out, "USB off | KB MS") == 0);  /* badge_text_lists_usb_and_inputs */
        for (unsigned m = 0; m < 8; ++m) {
            static const char *const want[8] = {
                "USB off", "USB off | KB", "USB off | MS", "USB off | KB MS",
                "USB on", "USB on | KB", "USB on | MS", "USB on | KB MS"};
            memset(&s, 0, sizeof s);
            s.usb_connected = (m & 4u) != 0; s.input_kbd = (m & 1u) != 0;
            s.input_mouse = (m & 2u) != 0;
            kvm_display_badge_text(&s, out, sizeof out);
            assert(strcmp(out, want[m]) == 0);
        }
        /* Buffer edges: always terminated, truncated, never overrun. */
        s.usb_connected = s.input_kbd = s.input_mouse = true;
        char guard[8]; memset(guard, 'x', sizeof guard);
        kvm_display_badge_text(&s, guard, 0);
        assert(guard[0] == 'x');
        kvm_display_badge_text(&s, guard, 1);
        assert(guard[0] == '\0' && guard[1] == 'x');
        kvm_display_badge_text(&s, guard, 5);
        assert(strcmp(guard, "USB ") == 0 && guard[5] == 'x');
        memset(guard, 'x', sizeof guard);
        kvm_display_badge_text(NULL, guard, sizeof guard);
        assert(guard[0] == '\0');
        kvm_display_badge_text(&s, NULL, 4);
    }

    kvm_display_buttons_t b;
    kvm_display_buttons_init(&b, false, true, 0);
    assert(kvm_display_buttons_sample(&b, false, true, 2000) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2010) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2045) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, true, false, 2100) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, true, false, 2135) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2200) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, false, 2235) == KVM_DISPLAY_NEXT_REQUEST);
    assert(kvm_display_buttons_sample(&b, false, true, 2300) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, true, 2335) == KVM_DISPLAY_NO_EVENT);
    assert(kvm_display_buttons_sample(&b, false, true, 3335) == KVM_DISPLAY_EMERGENCY_RELEASE);
    assert(kvm_display_buttons_sample(&b, false, true, 4335) == KVM_DISPLAY_NO_EVENT);

    kvm_display_pairing_t pairing = {0};
    kvm_display_pairing_started(&pairing, true, 1000);
    assert(pairing.local_owner && pairing.state == KVM_DISPLAY_PAIRING_WAITING);
    assert(pairing.deadline_ms == 61000);
    assert(kvm_display_pairing_event(&pairing, KVM_DISPLAY_PAIRING_CHALLENGE,
                                     91, 123456, 61000, 2000));
    request = (kvm_display_pair_request_t){KVM_DISPLAY_PAIR_APPROVE, 90};
    assert(!kvm_display_pairing_accept(&pairing, request, 2000));
    request.challenge_id = 91;
    assert(kvm_display_pairing_accept(&pairing, request, 2000));
    pairing.local_owner = false;
    assert(kvm_display_pairing_accept(&pairing, request, 2000));
    assert(!kvm_display_pairing_accept(&pairing, request, 61000));
    kvm_display_pairing_replied(&pairing, true);
    assert(pairing.state == KVM_DISPLAY_PAIRING_WAITING && pairing.challenge_id == 0);
    assert(!kvm_display_pairing_accept(&pairing, request, 2000));
    kvm_display_pairing_expire(&pairing, 61000);
    assert(pairing.state == KVM_DISPLAY_PAIRING_TIMEOUT && pairing.number == 0);
    kvm_display_pairing_clear(&pairing);
    assert(pairing.state == KVM_DISPLAY_PAIRING_CLOSED && !pairing.local_owner);
    input_pairing_tests();
    input_devices_tests();
    return 0;
}
