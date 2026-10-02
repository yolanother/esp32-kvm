/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the portable, fail-closed HID pairing policy. The policy admits
 * known bond identities or new peers during an explicit 60-second window,
 * requires numeric confirmation, and keeps eight opaque bond tokens stable. */
#ifndef ESP32_KVM_HID_PAIRING_H
#define ESP32_KVM_HID_PAIRING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_PAIRING_MAX_BONDS 8
#define HID_PAIRING_WINDOW_MS 60000ULL
#define HID_PAIRING_TOKEN_LEN 16

/** A resolved BLE peer identity; never use a rotating over-the-air address. */
typedef struct { uint8_t type; uint8_t address[6]; } hid_peer_t;
/** Exactly 16 opaque bytes in protocol v1 STATUS and FORGET_BOND. */
typedef struct { uint8_t bytes[HID_PAIRING_TOKEN_LEN]; } hid_token_t;
/** An identity and opaque application token persisted separately from BLE keys. */
typedef struct { hid_peer_t peer; hid_token_t token; } hid_bond_t;
/** Pairing policy state; only bonds are persisted. */
typedef struct {
    hid_bond_t bonds[HID_PAIRING_MAX_BONDS];
    size_t bond_count;
    uint64_t deadline_ms;
    uint16_t challenge_handle;
    uint32_t challenge_number;
    bool window_active;
    bool challenge_active;
    bool challenge_approved;
} hid_pairing_t;

/** Initializes an empty, closed policy. */
void hid_pairing_init(hid_pairing_t *state);
/** Opens a 60-second window if capacity remains; returns false if full. */
bool hid_pairing_open(hid_pairing_t *state, uint64_t now_ms);
/** Closes the window and cancels any pending challenge. */
void hid_pairing_cancel(hid_pairing_t *state);
/** Reports whether a pairing window has not expired. */
bool hid_pairing_window_open(hid_pairing_t *state, uint64_t now_ms);
/** Admits a known identity or a new peer within a pairing window. */
bool hid_pairing_admit(hid_pairing_t *state, hid_peer_t peer, uint64_t now_ms);
/** Begins a numeric comparison challenge for an admitted, unknown peer. */
bool hid_pairing_begin_challenge(hid_pairing_t *state, hid_peer_t peer, uint16_t handle,
                                 uint32_t number, uint64_t now_ms);
/** Accepts one matching challenge before expiry; rejection closes it. */
bool hid_pairing_confirm(hid_pairing_t *state, uint16_t handle, bool approved, uint64_t now_ms);
/** Reports whether a challenge awaits user confirmation. */
bool hid_pairing_challenge_pending(const hid_pairing_t *state);
/** Consumes approval for the specified connection after BLE authentication. */
bool hid_pairing_consume_approval(hid_pairing_t *state, uint16_t handle);
/** Adds a unique nonzero token for a new peer; never evicts an old bond. */
bool hid_pairing_add_bond(hid_pairing_t *state, hid_peer_t peer, hid_token_t token);
/** Copies a peer's opaque token; returns false if no bond exists. */
bool hid_pairing_token(const hid_pairing_t *state, hid_peer_t peer, hid_token_t *out);
/** Forgets a token only after explicit confirmation. */
bool hid_pairing_forget(hid_pairing_t *state, hid_token_t token, bool confirmed);

#endif
