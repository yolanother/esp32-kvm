/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the pure input hub: it routes locally attached input device reports
 * to one selected guest target and yields to the host-armed route. It holds
 * no platform state; the caller serializes all calls with the router actor. */
#ifndef ESP32_KVM_INPUT_HUB_H
#define ESP32_KVM_INPUT_HUB_H

#include "router.h"

/** Highest selectable target; 0 is the local sink, 1 to 3 are guest slots. */
#define KVM_INPUT_HUB_MAX_TARGET 3u

/** Outcome of delivering one device report. */
typedef enum {
    KVM_HUB_FORWARDED,
    KVM_HUB_SWALLOWED,
    KVM_HUB_DROPPED_HOST_ARMED,
    KVM_HUB_DROPPED_NOT_READY,
    KVM_HUB_TARGET_CHANGED,
    /** A chord was swallowed but releasing the held old state failed on a ready slot; it is
     * retried on each later swallowed keyboard report until it succeeds. */
    KVM_HUB_SWITCH_FAILED
} kvm_hub_result_t;

/** All guest slots: the live mask a hub starts with (nothing is known disconnected). */
#define KVM_INPUT_HUB_ALL_LIVE 0x07u

/** Hub state; armed_slot is zero while the hub holds no output arm. keys, buttons and
 * consumer are the last forwarded held state and stay set until an all-up is delivered.
 * want_* is the state the armed guest should hold (the device's latest state, zero after a
 * loss or a swallowed chord); dirty means a send was dropped or refused and want_* has not
 * reached the armed guest yet (kvm_input_hub_flush resends it). */
typedef struct {
    kvm_router_output_t output;
    void *context;
    uint8_t target;
    uint8_t armed_slot;
    uint8_t keys[8];
    uint8_t buttons;
    uint16_t consumer;
    uint8_t raw_keys[8]; /* last raw keyboard report, for chord press detection */
    bool chord_latched;  /* swallow keyboard reports until every non-modifier key is up */
    bool switch_pending; /* a chord switch or release is awaiting retry */
    uint8_t pending_target;
    /* Bit (slot - 1) clear: the slot is known disconnected (or known not ready on the HID
     * host, where no channel can stay armed). Only then is an arm forgotten unconfirmed. */
    uint8_t live_mask;
    bool dirty;
    uint8_t want_keys[8];
    uint8_t want_buttons;
    uint16_t want_consumer;
    /* An explicit choice (chord, touch, PLUS: any set_target) was made since init; until
     * then kvm_input_hub_auto_target keeps the target on a ready guest. Not persisted. */
    bool explicit_target;
} kvm_input_hub_t;

/** Initializes the hub on target zero with nothing armed or held, every slot live and no
 * explicit target (automatic targeting on). */
void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context);
/** Re-arm policy: a retry never arms. All-up is only ever sent to a slot the hub itself holds
 * armed (held state implies an arm). Whenever router_armed is true, or the router arms, the
 * hub abandons its held state and arm without emitting, so no retry can touch a slot the host
 * route owns. A false ready() alone is NOT proof the slot is gone: the request bridge also
 * answers false while an earlier timed-out request still holds the host loop, and then the
 * channel is still armed and holding keys. So the hub forgets an arm or held state without a
 * confirmed release only for a slot known disconnected (kvm_input_hub_set_live); a not-ready
 * or failing live slot keeps both, marks the hub dirty and is retried.
 *
 * Selects target 0 to 3, sending all-up and releasing the old target first. A valid target
 * is an explicit choice (it ends automatic targeting until init), even when the switch has
 * to be retried. Returns false and keeps the old target when the target is invalid or the
 * all-up or release did not go through on a live old target (retry later). A
 * known-disconnected old target never blocks the switch. */
bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target, bool router_armed);
/** Default target while This PC (0) is a stub: until the first explicit choice since init,
 * keeps the target on a ready guest. When the current target is not a ready guest in
 * ready_mask (bit slot - 1) it moves to the lowest-numbered ready one; a target that is still
 * ready is kept (no jump when a lower guest connects later, so a flapping guest cannot pull
 * the keyboard away), and with no ready guest the target is left alone. Returns true when the
 * target changed; a switch that could not release the old target returns false and the next
 * call retries it. */
bool kvm_input_hub_auto_target(kvm_input_hub_t *hub, uint8_t ready_mask, bool router_armed);
/** Records which guest slots may still hold an arm (bit slot - 1 set). Clearing the armed
 * slot's bit (link dropped, or the HID host reports it not ready) abandons its arm and held
 * state without emitting: a disconnected or unready channel is not armed. A successful hub
 * arm marks its slot live again. */
void kvm_input_hub_set_live(kvm_input_hub_t *hub, uint8_t live_mask);
/** Resends the wanted held state (want_*) to the armed slot after a drop or refusal (dirty).
 * Returns true when nothing is left to resend. With router_armed it abandons local state; a
 * known-disconnected slot is abandoned; a live slot that is not ready keeps it dirty. */
bool kvm_input_hub_flush(kvm_input_hub_t *hub, bool router_armed);
/** Delivers one report; arms the target lazily. While router_armed it drops the report,
 * clears local hub state and emits nothing. Keyboard reports with exactly Left Ctrl + Left Alt
 * (modifier byte 0x05) and a single newly pressed key select a target: 1 to 3 guests, 0 This
 * PC, Esc release all to This PC. The chord is swallowed whole, including the held key until
 * all non-modifier keys are up. Returns TARGET_CHANGED when the target changed, SWALLOWED otherwise, and
 * SWITCH_FAILED when releasing a ready old target failed: the switch is retried once per later
 * swallowed report; the report with all keys up releases the swallow, makes a final retry and
 * then abandons the switch (held state and arm are kept), so a new chord always works. */
kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input,
                                     bool router_armed);
/** Sends all-up then releases the arm. The caller must invoke it BEFORE the router arms;
 * the hub cannot see the router slot. Returns true when nothing is left held or armed
 * (a dead slot counts as released), false when a ready slot failed and a retry is needed.
 * With router_armed it abandons local state without emitting and returns true. */
bool kvm_input_hub_yield(kvm_input_hub_t *hub, bool router_armed);
/** Sends all-up keyboard, zero pointer and zero consumer after an input device is lost.
 * With router_armed it only abandons local state and emits nothing. */
void kvm_input_hub_device_lost(kvm_input_hub_t *hub, bool router_armed);
/** Releases only what one lost input device could hold, so the other device keeps
 * working: KVM_ROUTER_KEYBOARD (a keyboard) sends all-up keyboard plus zero
 * consumer when held and resets chord tracking; KVM_ROUTER_POINTER (a mouse)
 * sends zero pointer state. Any other kind is a whole loss, exactly like
 * kvm_input_hub_device_lost. The arm is released when nothing at all was held,
 * and with router_armed it only abandons local state and emits nothing. */
void kvm_input_hub_device_lost_kind(kvm_input_hub_t *hub, kvm_router_input_kind_t kind,
                                    bool router_armed);

#endif
