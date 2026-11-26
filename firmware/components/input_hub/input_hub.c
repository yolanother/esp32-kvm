/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the pure input hub. The target arm is taken on the first forwarded
 * report and dropped on every target change, yield and idle device loss, so the
 * hub never holds an output the host route may need. The hub never emits while
 * the router is armed and never arms to retry. A refused or not-ready request on
 * a live slot keeps the arm and held state (the channel may still hold keys) and
 * marks the hub dirty for a resend; only a slot known disconnected is abandoned.
 * Keyboard chords (Left Ctrl + Left Alt + key) switch the target. Guests and This
 * PC (once a PC output is bound) share one state machine through a channel view:
 * which output, context and callback slot a target uses. */
#include "input_hub.h"
#include <string.h>

#define MOD_LCTRL_LALT 0x05u
#define HID_KEY_1 0x1Eu
#define HID_KEY_3 0x20u
#define HID_KEY_0 0x27u
#define HID_KEY_ESC 0x29u

/* The output a target goes through: a guest uses the hub output at its own slot,
 * This PC uses the PC output at slot 0. */
typedef struct {
    const kvm_router_output_t *out;
    void *context;
    uint8_t slot;
    bool pc;
} hub_channel_t;

/* False for the stub target 0 (no PC output bound): it has no output at all. */
static bool target_channel(const kvm_input_hub_t *hub, uint8_t target, hub_channel_t *ch)
{
    if (target == 0) {
        if (!hub->pc_enabled) {
            return false;
        }
        ch->out = &hub->pc_output;
        ch->context = hub->pc_context;
        ch->slot = 0;
        ch->pc = true;
        return true;
    }
    ch->out = &hub->output;
    ch->context = hub->context;
    ch->slot = target;
    ch->pc = false;
    return true;
}

/* The channel the hub holds armed, if any (at most one is). */
static bool armed_channel(const kvm_input_hub_t *hub, hub_channel_t *ch)
{
    if (hub->pc_armed) {
        return target_channel(hub, 0, ch);
    }
    return hub->armed_slot != 0 && target_channel(hub, hub->armed_slot, ch);
}

static bool channel_armed(const kvm_input_hub_t *hub, const hub_channel_t *ch)
{
    return ch->pc ? hub->pc_armed : hub->armed_slot == ch->slot;
}

static void mark_armed(kvm_input_hub_t *hub, const hub_channel_t *ch)
{
    if (ch->pc) {
        hub->pc_armed = true;
        hub->pc_live = true; /* the arm proves it is live */
    } else {
        hub->armed_slot = ch->slot;
        hub->live_mask |= (uint8_t)(1u << (ch->slot - 1u)); /* the arm proves it is live */
    }
}

static void forget_arm(kvm_input_hub_t *hub)
{
    hub->armed_slot = 0;
    hub->pc_armed = false;
}

static bool channel_ready(const hub_channel_t *ch)
{
    return ch->out->ready != NULL && ch->out->ready(ch->context, ch->slot);
}

static bool keys_held(const kvm_input_hub_t *hub)
{
    static const uint8_t none[8] = {0};
    return memcmp(hub->keys, none, sizeof none) != 0;
}

static bool held(const kvm_input_hub_t *hub)
{
    return keys_held(hub) || hub->buttons != 0 || hub->consumer != 0;
}

static void clear_held(kvm_input_hub_t *hub)
{
    memset(hub->keys, 0, sizeof hub->keys);
    hub->buttons = 0;
    hub->consumer = 0;
}

static void clear_want(kvm_input_hub_t *hub)
{
    memset(hub->want_keys, 0, sizeof hub->want_keys);
    hub->want_buttons = 0;
    hub->want_consumer = 0;
}

static void clear_local(kvm_input_hub_t *hub)
{
    clear_held(hub);
    clear_want(hub);
    forget_arm(hub);
    memset(hub->raw_keys, 0, sizeof hub->raw_keys);
    hub->chord_latched = false;
    hub->switch_pending = false;
    hub->dirty = false;
}

/* True only when the slot is known disconnected (or known not ready on the HID
 * host): its channel cannot be armed, so it cannot hold keys down. A false
 * ready() is not enough; the request bridge refuses while it is busy. */
static bool slot_gone(const kvm_input_hub_t *hub, uint8_t slot)
{
    return slot >= 1 && slot <= KVM_INPUT_HUB_MAX_TARGET &&
           (hub->live_mask & (1u << (slot - 1u))) == 0;
}

/* The channel form: a guest by its live bit, the PC by pc_live (unmounted). */
static bool channel_gone(const kvm_input_hub_t *hub, const hub_channel_t *ch)
{
    return ch->pc ? !hub->pc_live : slot_gone(hub, ch->slot);
}

/* Forgets the arm and held state of a slot known disconnected, without emitting. */
static void abandon_gone(kvm_input_hub_t *hub)
{
    clear_held(hub);
    forget_arm(hub);
    hub->dirty = false;
}

#define UP_KEYS 1u
#define UP_POINTER 2u
#define UP_CONSUMER 4u
#define UP_ALL (UP_KEYS | UP_POINTER | UP_CONSUMER)

static void clear_parts(kvm_input_hub_t *hub, unsigned parts)
{
    if (parts & UP_KEYS) memset(hub->keys, 0, sizeof hub->keys);
    if (parts & UP_POINTER) hub->buttons = 0;
    if (parts & UP_CONSUMER) hub->consumer = 0;
}

/* Sends the selected all-up parts to the armed channel: keyboard all-up and zero pointer
 * always, zero consumer only when held, in that order. Each part clears its held state only
 * when its send succeeds. Never sends to an unarmed channel. A known-disconnected channel has
 * the selected held state abandoned and counts as success. A live channel that is not ready
 * or refuses keeps the state and marks the hub dirty. Returns true when none of the selected
 * state is left. */
static bool send_up(kvm_input_hub_t *hub, unsigned parts)
{
    kvm_router_input_t up;
    hub_channel_t ch;
    bool ok = true;
    if (!armed_channel(hub, &ch) || channel_gone(hub, &ch)) {
        clear_parts(hub, parts);
        return true;
    }
    if (ch.out->send == NULL || !channel_ready(&ch)) {
        hub->dirty = true; /* busy or not ready: the channel may still hold the keys */
        return false;
    }
    if (parts & UP_KEYS) {
        memset(&up, 0, sizeof up);
        up.kind = KVM_ROUTER_KEYBOARD;
        if (ch.out->send(ch.context, ch.slot, &up)) {
            memset(hub->keys, 0, sizeof hub->keys);
        } else {
            ok = false;
        }
    }
    if (parts & UP_POINTER) {
        memset(&up, 0, sizeof up);
        up.kind = KVM_ROUTER_POINTER;
        if (ch.out->send(ch.context, ch.slot, &up)) {
            hub->buttons = 0;
        } else {
            ok = false;
        }
    }
    if ((parts & UP_CONSUMER) && hub->consumer != 0) {
        memset(&up, 0, sizeof up);
        up.kind = KVM_ROUTER_CONSUMER;
        if (ch.out->send(ch.context, ch.slot, &up)) {
            hub->consumer = 0;
        } else {
            ok = false;
        }
    }
    if (!ok) {
        hub->dirty = true;
    }
    return ok;
}

static bool send_all_up(kvm_input_hub_t *hub) { return send_up(hub, UP_ALL); }

/* Releases the arm; keeps it when a live channel's release is not confirmed, so the
 * channel is never left armed behind the hub's back (an armed channel refuses the next arm). */
static bool drop_arm(kvm_input_hub_t *hub)
{
    hub_channel_t ch;
    if (!armed_channel(hub, &ch)) {
        return true;
    }
    if (ch.out->release != NULL && !ch.out->release(ch.context, ch.slot) &&
        !channel_gone(hub, &ch)) {
        return false;
    }
    forget_arm(hub);
    return true;
}

/* Ends the arm for a switch or a yield: every all-up first, the release only after them.
 * One exception, the PC only: a live PC that is not ready (suspended) cannot take the all-up
 * now and may stay asleep for hours, so its release, which zeroes the state-based HID sink for
 * delivery on resume, stands in for the all-up. A sleeping PC thus never blocks a switch to a
 * guest; a ready PC that refuses, and every guest, keep the retry rule. */
static bool release_armed(kvm_input_hub_t *hub)
{
    hub_channel_t ch;
    if (send_all_up(hub)) {
        return drop_arm(hub);
    }
    if (!hub->pc_armed || !armed_channel(hub, &ch) || channel_ready(&ch)) {
        return false;
    }
    if (ch.out->release == NULL || !ch.out->release(ch.context, ch.slot)) {
        return false; /* not confirmed: keep the arm and held state, retry later */
    }
    abandon_gone(hub); /* the output owns the zero state now */
    return true;
}

void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context)
{
    memset(hub, 0, sizeof *hub);
    hub->output = output;
    hub->context = context;
    hub->live_mask = KVM_INPUT_HUB_ALL_LIVE;
    hub->pc_live = true;
}

static bool switch_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed);

void kvm_input_hub_set_pc_output(kvm_input_hub_t *hub, kvm_router_output_t pc, void *pc_context)
{
    hub->pc_output = pc;
    hub->pc_context = pc_context;
    hub->pc_enabled = true;
    hub->explicit_target = true; /* composite mode defaults to This PC as a choice */
    /* Bound before the router can arm, so releasing a guest arm here is the usual switch. */
    (void)switch_target(hub, 0, false);
}

void kvm_input_hub_set_pc_live(kvm_input_hub_t *hub, bool live)
{
    hub->pc_live = live;
    if (!live && hub->pc_armed) {
        abandon_gone(hub);
    }
}

void kvm_input_hub_set_live(kvm_input_hub_t *hub, uint8_t live_mask)
{
    hub->live_mask = (uint8_t)(live_mask & KVM_INPUT_HUB_ALL_LIVE);
    if (hub->armed_slot != 0 && slot_gone(hub, hub->armed_slot)) {
        abandon_gone(hub);
    }
}

static bool send_state(kvm_input_hub_t *hub, const hub_channel_t *ch, kvm_router_input_kind_t kind)
{
    kvm_router_input_t in;
    memset(&in, 0, sizeof in);
    in.kind = kind;
    memcpy(in.keyboard, hub->want_keys, sizeof in.keyboard);
    in.buttons = hub->want_buttons;
    in.consumer = hub->want_consumer;
    if (!ch->out->send(ch->context, ch->slot, &in)) {
        return false;
    }
    if (kind == KVM_ROUTER_KEYBOARD) {
        memcpy(hub->keys, hub->want_keys, sizeof hub->keys);
    } else if (kind == KVM_ROUTER_POINTER) {
        hub->buttons = hub->want_buttons;
    } else {
        hub->consumer = hub->want_consumer;
    }
    return true;
}

bool kvm_input_hub_flush(kvm_input_hub_t *hub, bool router_armed)
{
    hub_channel_t ch;
    bool ok = true;
    if (!hub->dirty) {
        return true;
    }
    if (router_armed) {
        clear_local(hub);
        return true;
    }
    if (!armed_channel(hub, &ch)) {
        hub->dirty = false; /* nothing armed: no output can hold the hub's keys */
        return true;
    }
    if (channel_gone(hub, &ch)) {
        abandon_gone(hub);
        return true;
    }
    if (ch.out->send == NULL || !channel_ready(&ch)) {
        return false;
    }
    if (memcmp(hub->keys, hub->want_keys, sizeof hub->keys) != 0) {
        ok = send_state(hub, &ch, KVM_ROUTER_KEYBOARD) && ok;
    }
    if (hub->buttons != hub->want_buttons) {
        ok = send_state(hub, &ch, KVM_ROUTER_POINTER) && ok;
    }
    if (hub->consumer != hub->want_consumer) {
        ok = send_state(hub, &ch, KVM_ROUTER_CONSUMER) && ok;
    }
    hub->dirty = !ok;
    return ok;
}

bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed)
{
    if (target > KVM_INPUT_HUB_MAX_TARGET) {
        return false;
    }
    hub->explicit_target = true;
    return switch_target(hub, target, router_armed);
}

bool kvm_input_hub_auto_target(kvm_input_hub_t *hub, uint8_t ready_mask, bool router_armed)
{
    uint8_t slot;
    if (hub->explicit_target || hub->pc_enabled) {
        return false;
    }
    if (hub->target != 0 && (ready_mask & (1u << (hub->target - 1u)))) {
        return false; /* still on a ready guest: keep it */
    }
    for (slot = 1; slot <= KVM_INPUT_HUB_MAX_TARGET; slot++) {
        if (ready_mask & (1u << (slot - 1u))) {
            break;
        }
    }
    if (slot > KVM_INPUT_HUB_MAX_TARGET || slot == hub->target) {
        return false;
    }
    return switch_target(hub, slot, router_armed);
}

static bool switch_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed)
{
    if (router_armed) {
        clear_local(hub);
        hub->target = target;
        return true;
    }
    if (target == hub->target) {
        hub->switch_pending = false;
        return true;
    }
    if (!release_armed(hub)) {
        return false;
    }
    hub->target = target;
    hub->switch_pending = false;
    return true;
}

/* Decides whether a keyboard report is a chord press. Exactly Left Ctrl + Left Alt and exactly
 * one non-modifier key; sets the target and whether it also releases everything. */
static bool hub_chord(const uint8_t keys[8], uint8_t *target, bool *release_all)
{
    uint8_t code = 0;
    int i;
    if (keys[0] != MOD_LCTRL_LALT || keys[1] != 0) {
        return false;
    }
    for (i = 2; i < 8; i++) {
        if (keys[i] != 0) {
            if (code != 0) {
                return false;
            }
            code = keys[i];
        }
    }
    *release_all = false;
    if (code >= HID_KEY_1 && code <= HID_KEY_3) {
        *target = (uint8_t)(code - HID_KEY_1 + 1u);
    } else if (code == HID_KEY_0) {
        *target = 0;
    } else if (code == HID_KEY_ESC) {
        *target = 0;
        *release_all = true;
    } else {
        return false;
    }
    return true;
}

static bool apply_switch(kvm_input_hub_t *hub, uint8_t target)
{
    if (target != hub->target) {
        return kvm_input_hub_set_target(hub, target, false);
    }
    /* Unchanged: still release anything held (modifiers forwarded before the chord). */
    return kvm_input_hub_yield(hub, false);
}

static bool no_keys(const uint8_t keys[8])
{
    int i;
    for (i = 2; i < 8; i++) {
        if (keys[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Handles a keyboard report for chords. Returns true and sets *result when consumed. */
static bool chord_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                        kvm_hub_result_t *result)
{
    uint8_t target = 0;
    bool release_all = false;
    uint8_t before = hub->target;
    bool was_up = no_keys(hub->raw_keys);
    memcpy(hub->raw_keys, input->keyboard, sizeof hub->raw_keys);
    if (hub->chord_latched) {
        *result = KVM_HUB_SWALLOWED;
        if (hub->switch_pending) {
            uint8_t prior = hub->target;
            if (!apply_switch(hub, hub->pending_target)) {
                *result = KVM_HUB_SWITCH_FAILED;
            } else {
                hub->switch_pending = false;
                if (hub->target != prior) {
                    *result = KVM_HUB_TARGET_CHANGED;
                }
            }
        }
        if (no_keys(input->keyboard)) {
            /* All keys up always ends the swallow; an unresolved switch is abandoned (held
             * state and arm are kept for device_lost or a new chord to retry). */
            hub->chord_latched = false;
            hub->switch_pending = false;
        }
        return true;
    }
    /* A chord key must be newly pressed with nothing else held. */
    if (!was_up || !hub_chord(input->keyboard, &target, &release_all)) {
        return false;
    }
    hub->chord_latched = true;
    hub->explicit_target = true; /* even a chord for the current target is a choice */
    hub->switch_pending = false; /* a fresh chord supersedes any stale pending target */
    *result = KVM_HUB_SWALLOWED;
    if (!apply_switch(hub, target)) {
        hub->switch_pending = true;
        hub->pending_target = target;
        *result = KVM_HUB_SWITCH_FAILED;
    } else if (target != before) {
        *result = KVM_HUB_TARGET_CHANGED;
    }
    return true;
}

/* A report while the router is armed, with a PC output bound: local state is cleared and
 * nothing is emitted, as without one, but chord tracking survives so a chord can be
 * reported to the host (and stays swallowed until all keys are up, even once the router
 * disarms). Without a PC output this is not reached: stage 2 forgets chord state here. */
static kvm_hub_result_t host_armed_input(kvm_input_hub_t *hub, const kvm_router_input_t *input)
{
    uint8_t raw[8];
    bool latched = hub->chord_latched;
    bool chord = false;
    uint8_t target = 0;
    bool release_all = false;
    memcpy(raw, hub->raw_keys, sizeof raw);
    clear_local(hub);
    if (input->kind != KVM_ROUTER_KEYBOARD) {
        memcpy(hub->raw_keys, raw, sizeof hub->raw_keys);
        hub->chord_latched = latched;
        return KVM_HUB_DROPPED_HOST_ARMED;
    }
    memcpy(hub->raw_keys, input->keyboard, sizeof hub->raw_keys);
    if (latched) {
        hub->chord_latched = !no_keys(input->keyboard); /* all keys up ends the swallow */
    } else if (no_keys(raw) && hub_chord(input->keyboard, &target, &release_all)) {
        hub->chord_latched = true;
        hub->host_request_target = target; /* Esc and 0 both give 0 */
        chord = true;
    }
    return chord ? KVM_HUB_CHORD_HOST_ARMED : KVM_HUB_DROPPED_HOST_ARMED;
}

kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed)
{
    kvm_hub_result_t chord_result;
    hub_channel_t ch;
    if (router_armed) {
        if (hub->pc_enabled) {
            return host_armed_input(hub, input);
        }
        /* The host route owns the output: emit nothing, forget local state. */
        clear_local(hub);
        return KVM_HUB_DROPPED_HOST_ARMED;
    }
    if (input->kind == KVM_ROUTER_KEYBOARD && chord_input(hub, input, &chord_result)) {
        /* The chord is swallowed whole: the target should hold no keyboard state. */
        memset(hub->want_keys, 0, sizeof hub->want_keys);
        return chord_result;
    }
    if (input->kind == KVM_ROUTER_KEYBOARD) {
        memcpy(hub->want_keys, input->keyboard, sizeof hub->want_keys);
    } else if (input->kind == KVM_ROUTER_POINTER) {
        hub->want_buttons = input->buttons;
    } else if (input->kind == KVM_ROUTER_CONSUMER) {
        hub->want_consumer = input->consumer;
    }
    if (!target_channel(hub, hub->target, &ch)) {
        return KVM_HUB_FORWARDED; /* the stage-2 This PC stub swallows it */
    }
    if (!channel_ready(&ch)) {
        /* A live armed output may still hold the previous state: resend later. */
        if (channel_armed(hub, &ch)) hub->dirty = true;
        return KVM_HUB_DROPPED_NOT_READY;
    }
    if (!channel_armed(hub, &ch)) {
        if (ch.out->arm == NULL || !ch.out->arm(ch.context, ch.slot)) {
            return KVM_HUB_DROPPED_NOT_READY;
        }
        mark_armed(hub, &ch);
    }
    if (ch.out->send == NULL || !ch.out->send(ch.context, ch.slot, input)) {
        hub->dirty = true;
        return KVM_HUB_DROPPED_NOT_READY;
    }
    if (input->kind == KVM_ROUTER_KEYBOARD) {
        memcpy(hub->keys, input->keyboard, sizeof hub->keys);
    } else if (input->kind == KVM_ROUTER_POINTER) {
        hub->buttons = input->buttons;
    } else if (input->kind == KVM_ROUTER_CONSUMER) {
        hub->consumer = input->consumer;
    }
    return KVM_HUB_FORWARDED;
}

bool kvm_input_hub_yield(kvm_input_hub_t *hub, bool router_armed)
{
    if (router_armed) {
        clear_local(hub);
        return true;
    }
    /* A failed all-up keeps the arm too, so a retry can still reach the held output. */
    return release_armed(hub);
}

void kvm_input_hub_device_lost(kvm_input_hub_t *hub, bool router_armed)
{
    bool was_held;
    if (router_armed) {
        clear_local(hub);
        return;
    }
    memset(hub->raw_keys, 0, sizeof hub->raw_keys);
    hub->chord_latched = false;
    hub->switch_pending = false;
    clear_want(hub); /* a lost device holds nothing: a refused all-up is resent by flush */
    was_held = held(hub);
    if (send_all_up(hub) && !was_held) {
        (void)drop_arm(hub);
    }
}

void kvm_input_hub_device_lost_kind(kvm_input_hub_t *hub, kvm_router_input_kind_t kind,
                                    bool router_armed)
{
    bool was_held;
    unsigned parts;
    if (kind == KVM_ROUTER_KEYBOARD) {
        parts = UP_KEYS | UP_CONSUMER;
    } else if (kind == KVM_ROUTER_POINTER) {
        parts = UP_POINTER;
    } else {
        kvm_input_hub_device_lost(hub, router_armed);
        return;
    }
    if (router_armed) {
        clear_local(hub);
        return;
    }
    if (parts & UP_KEYS) {
        /* Chord tracking belongs to the keyboard that went away. */
        memset(hub->raw_keys, 0, sizeof hub->raw_keys);
        hub->chord_latched = false;
        hub->switch_pending = false;
        memset(hub->want_keys, 0, sizeof hub->want_keys);
    }
    if (parts & UP_POINTER) hub->want_buttons = 0;
    if (parts & UP_CONSUMER) hub->want_consumer = 0;
    was_held = held(hub);
    if (send_up(hub, parts) && !was_held) {
        (void)drop_arm(hub);
    }
}
