/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks the portable HID pairing policy for timed admission, explicit numeric
 * confirmation, stable bond tokens, full capacity, and deliberate forgetting. */
#include <assert.h>
#include <string.h>
#include "hid_pairing.h"

static hid_peer_t peer(unsigned value)
{
    hid_peer_t result = {0};
    result.address[0] = (uint8_t)value;
    result.type = 1;
    return result;
}

static hid_token_t token(unsigned value)
{
    hid_token_t result = {{0}};
    result.bytes[0] = (uint8_t)value;
    result.bytes[15] = (uint8_t)(value ^ 0xa5);
    return result;
}

int main(void)
{
    hid_pairing_t state;
    hid_pairing_init(&state);
    hid_token_t zero = {{0}};
    assert(sizeof(zero.bytes) == 16);
    assert(!hid_pairing_add_bond(&state, peer(1), zero));
    assert(!hid_pairing_admit(&state, peer(1), 0));
    assert(hid_pairing_open(&state, 1000));
    assert(hid_pairing_admit(&state, peer(1), 59999));
    assert(!hid_pairing_admit(&state, peer(1), 61000));
    assert(!hid_pairing_window_open(&state, 61000));
    assert(hid_pairing_open(&state, 70000));
    assert(hid_pairing_begin_challenge(&state, peer(1), 17, 0x12345678, 123456, 70001));
    assert(!hid_pairing_confirm(&state, 0x12345679, true, 70002));
    assert(hid_pairing_confirm(&state, 0x12345678, true, 70002));
    assert(!hid_pairing_challenge_pending(&state));
    assert(hid_pairing_consume_approval(&state, 17));
    assert(!hid_pairing_consume_approval(&state, 17));
    hid_token_t found;
    assert(hid_pairing_add_bond(&state, peer(1), token(1)));
    assert(!hid_pairing_add_bond(&state, peer(2), token(1)));
    assert(hid_pairing_token(&state, peer(1), &found));
    hid_token_t expected = token(1);
    assert(memcmp(found.bytes, expected.bytes, HID_PAIRING_TOKEN_LEN) == 0);
    hid_pairing_cancel(&state);
    assert(hid_pairing_admit(&state, peer(1), 80000));
    assert(!hid_pairing_admit(&state, peer(2), 80000));
    for (unsigned index = 2; index <= HID_PAIRING_MAX_BONDS; ++index)
        assert(hid_pairing_add_bond(&state, peer(index), token(index)));
    assert(!hid_pairing_open(&state, 90000));
    assert(!hid_pairing_add_bond(&state, peer(9), token(9)));
    assert(!hid_pairing_forget(&state, token(1), false));
    assert(hid_pairing_token(&state, peer(1), &found));
    assert(hid_pairing_forget(&state, token(1), true));
    assert(!hid_pairing_admit(&state, peer(1), 90000));
    assert(hid_pairing_open(&state, 90000));
    assert(hid_pairing_begin_challenge(&state, peer(9), 19, 0xabcdef01, 654321, 90000));
    assert(!hid_pairing_confirm(&state, 0xabcdef01, true, 150000));
    assert(!hid_pairing_consume_approval(&state, 19));
    assert(!hid_pairing_challenge_pending(&state));
    assert(hid_pairing_open(&state, 160000));
    assert(!hid_pairing_begin_challenge(&state, peer(9), 19, 0xabcdef01, 654321, 160001));
    assert(!hid_pairing_begin_challenge(&state, peer(9), 19, 0, 654321, 160001));
    hid_pairing_t reloaded = state;
    assert(hid_pairing_token(&reloaded, peer(2), &found));
    expected = token(2);
    assert(memcmp(found.bytes, expected.bytes, HID_PAIRING_TOKEN_LEN) == 0);
    return 0;
}
