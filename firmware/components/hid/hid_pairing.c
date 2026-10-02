/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements timed HID admission, one pending numeric comparison, and a
 * bounded identity-to-token bond table without implicit deletion. */
#include "hid_pairing.h"
#include <string.h>

static bool same_peer(hid_peer_t a, hid_peer_t b)
{
    return a.type == b.type && memcmp(a.address, b.address, sizeof(a.address)) == 0;
}

void hid_pairing_init(hid_pairing_t *state) { memset(state, 0, sizeof(*state)); }

bool hid_pairing_open(hid_pairing_t *state, uint64_t now_ms)
{
    if (state->bond_count >= HID_PAIRING_MAX_BONDS) return false;
    state->deadline_ms = now_ms + HID_PAIRING_WINDOW_MS;
    state->window_active = true;
    state->challenge_active = false;
    state->challenge_approved = false;
    return true;
}

void hid_pairing_cancel(hid_pairing_t *state)
{
    state->window_active = false;
    state->challenge_active = false;
    state->challenge_approved = false;
}

bool hid_pairing_window_open(hid_pairing_t *state, uint64_t now_ms)
{
    if (state->window_active && now_ms >= state->deadline_ms) hid_pairing_cancel(state);
    return state->window_active;
}

uint64_t hid_pairing_token(const hid_pairing_t *state, hid_peer_t peer)
{
    for (size_t index = 0; index < state->bond_count; ++index)
        if (same_peer(state->bonds[index].peer, peer)) return state->bonds[index].token;
    return 0;
}

bool hid_pairing_admit(hid_pairing_t *state, hid_peer_t peer, uint64_t now_ms)
{
    if (hid_pairing_token(state, peer)) return true;
    return hid_pairing_window_open(state, now_ms) && state->bond_count < HID_PAIRING_MAX_BONDS;
}

bool hid_pairing_begin_challenge(hid_pairing_t *state, hid_peer_t peer, uint16_t handle,
                                 uint32_t number, uint64_t now_ms)
{
    if (!hid_pairing_window_open(state, now_ms) || hid_pairing_token(state, peer) ||
        state->challenge_active || number > 999999) return false;
    state->challenge_handle = handle;
    state->challenge_number = number;
    state->challenge_active = true;
    state->challenge_approved = false;
    return true;
}

bool hid_pairing_confirm(hid_pairing_t *state, uint16_t handle, bool approved, uint64_t now_ms)
{
    if (!hid_pairing_window_open(state, now_ms) || !state->challenge_active ||
        state->challenge_handle != handle) return false;
    state->challenge_active = false;
    state->challenge_approved = approved;
    return approved;
}

bool hid_pairing_challenge_pending(const hid_pairing_t *state) { return state->challenge_active; }

bool hid_pairing_consume_approval(hid_pairing_t *state, uint16_t handle)
{
    if (!state->challenge_approved || state->challenge_handle != handle) return false;
    state->challenge_approved = false;
    return true;
}

bool hid_pairing_add_bond(hid_pairing_t *state, hid_peer_t peer, uint64_t token)
{
    if (!token || state->bond_count >= HID_PAIRING_MAX_BONDS || hid_pairing_token(state, peer))
        return false;
    for (size_t index = 0; index < state->bond_count; ++index)
        if (state->bonds[index].token == token) return false;
    state->bonds[state->bond_count++] = (hid_bond_t){peer, token};
    hid_pairing_cancel(state);
    return true;
}

bool hid_pairing_forget(hid_pairing_t *state, uint64_t token, bool confirmed)
{
    if (!confirmed || !token) return false;
    for (size_t index = 0; index < state->bond_count; ++index) {
        if (state->bonds[index].token != token) continue;
        for (size_t next = index + 1; next < state->bond_count; ++next)
            state->bonds[next - 1] = state->bonds[next];
        --state->bond_count;
        memset(&state->bonds[state->bond_count], 0, sizeof(state->bonds[0]));
        return true;
    }
    return false;
}
