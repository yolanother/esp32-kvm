/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements timed HID admission, three numeric comparisons per window, and a
 * bounded identity-to-token bond table without implicit deletion. Connected
 * lookup clears output unless the peer is authenticated and bound. Inventory
 * copies retained opaque tokens without exposing peer identities. */
#include "hid_pairing.h"
#include <string.h>

static bool same_peer(hid_peer_t a, hid_peer_t b)
{
    return a.type == b.type && memcmp(a.address, b.address, sizeof(a.address)) == 0;
}

static bool token_zero(hid_token_t token)
{
    static const hid_token_t zero = {{0}};
    return memcmp(token.bytes, zero.bytes, HID_PAIRING_TOKEN_LEN) == 0;
}

static bool same_token(hid_token_t a, hid_token_t b)
{
    return memcmp(a.bytes, b.bytes, HID_PAIRING_TOKEN_LEN) == 0;
}

void hid_pairing_init(hid_pairing_t *state) { memset(state, 0, sizeof(*state)); }

bool hid_pairing_open(hid_pairing_t *state, uint64_t now_ms)
{
    if (state->bond_count >= HID_PAIRING_MAX_BONDS) return false;
    state->deadline_ms = now_ms + HID_PAIRING_WINDOW_MS;
    state->window_active = true;
    state->challenge_active = false;
    state->challenge_approved = false;
    state->challenge_id = 0;
    state->attempt_count = 0;
    return true;
}

void hid_pairing_cancel(hid_pairing_t *state)
{
    state->window_active = false;
    state->challenge_active = false;
    state->challenge_approved = false;
    state->challenge_id = 0;
}

bool hid_pairing_window_open(hid_pairing_t *state, uint64_t now_ms)
{
    if (state->window_active && now_ms >= state->deadline_ms) hid_pairing_cancel(state);
    return state->window_active;
}

bool hid_pairing_token(const hid_pairing_t *state, hid_peer_t peer, hid_token_t *out)
{
    for (size_t index = 0; index < state->bond_count; ++index)
        if (same_peer(state->bonds[index].peer, peer)) {
            *out = state->bonds[index].token;
            return true;
        }
    return false;
}

bool hid_pairing_admit(hid_pairing_t *state, hid_peer_t peer, uint64_t now_ms)
{
    hid_token_t token;
    if (hid_pairing_token(state, peer, &token)) return true;
    return hid_pairing_window_open(state, now_ms) && state->bond_count < HID_PAIRING_MAX_BONDS;
}

bool hid_pairing_begin_challenge(hid_pairing_t *state, hid_peer_t peer, uint16_t handle,
                                 uint32_t challenge_id, uint32_t number, uint64_t now_ms)
{
    hid_token_t token;
    if (!hid_pairing_window_open(state, now_ms) || hid_pairing_token(state, peer, &token) ||
        state->challenge_active || state->challenge_approved ||
        state->attempt_count >= HID_PAIRING_MAX_ATTEMPTS ||
        !challenge_id || challenge_id == state->last_challenge_id ||
        number > 999999) return false;
    state->challenge_handle = handle;
    state->challenge_id = challenge_id;
    state->last_challenge_id = challenge_id;
    state->challenge_number = number;
    state->challenge_active = true;
    state->challenge_approved = false;
    if (state->deadline_ms - now_ms < HID_PAIRING_CHALLENGE_MIN_MS)
        state->deadline_ms = now_ms + HID_PAIRING_CHALLENGE_MIN_MS;
    ++state->attempt_count;
    return true;
}

bool hid_pairing_confirm(hid_pairing_t *state, uint32_t challenge_id,
                         bool approved, uint64_t now_ms)
{
    if (!hid_pairing_window_open(state, now_ms) || !state->challenge_active ||
        !challenge_id || state->challenge_id != challenge_id) return false;
    state->challenge_active = false;
    state->challenge_approved = approved;
    state->challenge_id = 0;
    return approved;
}

bool hid_pairing_challenge_pending(const hid_pairing_t *state) { return state->challenge_active; }

bool hid_pairing_consume_approval(hid_pairing_t *state, uint16_t handle)
{
    if (!state->challenge_approved || state->challenge_handle != handle) return false;
    state->challenge_approved = false;
    return true;
}

bool hid_pairing_add_bond(hid_pairing_t *state, hid_peer_t peer, hid_token_t token)
{
    hid_token_t existing;
    if (token_zero(token) || state->bond_count >= HID_PAIRING_MAX_BONDS ||
        hid_pairing_token(state, peer, &existing))
        return false;
    for (size_t index = 0; index < state->bond_count; ++index)
        if (same_token(state->bonds[index].token, token)) return false;
    state->bonds[state->bond_count++] = (hid_bond_t){peer, token};
    hid_pairing_cancel(state);
    return true;
}

bool hid_pairing_connected_token(const hid_pairing_t *state, bool connected,
                                 bool authenticated, hid_peer_t peer, hid_token_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!connected || !authenticated || !state || !hid_pairing_token(state, peer, out) ||
        token_zero(*out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

bool hid_pairing_forget(hid_pairing_t *state, hid_token_t token, bool confirmed)
{
    if (!confirmed || token_zero(token)) return false;
    for (size_t index = 0; index < state->bond_count; ++index) {
        if (!same_token(state->bonds[index].token, token)) continue;
        for (size_t next = index + 1; next < state->bond_count; ++next)
            state->bonds[next - 1] = state->bonds[next];
        --state->bond_count;
        memset(&state->bonds[state->bond_count], 0, sizeof(state->bonds[0]));
        return true;
    }
    return false;
}

bool hid_pairing_inventory(const hid_pairing_t *state,
                           hid_token_t out[HID_PAIRING_MAX_BONDS], size_t *count)
{
    if (!out || !count) return false;
    memset(out, 0, HID_PAIRING_MAX_BONDS * sizeof(*out));
    *count = 0;
    if (!state || state->bond_count > HID_PAIRING_MAX_BONDS) return false;
    for (size_t i = 0; i < state->bond_count; ++i) {
        hid_token_t token = state->bonds[i].token;
        if (token_zero(token)) return false;
        for (size_t j = 0; j < i; ++j)
            if (same_token(out[j], token)) return false;
        out[i] = token;
    }
    *count = state->bond_count;
    return true;
}
