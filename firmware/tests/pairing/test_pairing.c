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

int main(void)
{
    hid_pairing_t state;
    hid_pairing_init(&state);
    assert(!hid_pairing_admit(&state, peer(1), 0));
    assert(hid_pairing_open(&state, 1000));
    assert(hid_pairing_admit(&state, peer(1), 59999));
    assert(!hid_pairing_admit(&state, peer(1), 61000));
    assert(!hid_pairing_window_open(&state, 61000));
    assert(hid_pairing_open(&state, 70000));
    assert(hid_pairing_begin_challenge(&state, peer(1), 17, 123456, 70001));
    assert(!hid_pairing_confirm(&state, 18, true, 70002));
    assert(hid_pairing_confirm(&state, 17, true, 70002));
    assert(!hid_pairing_challenge_pending(&state));
    assert(hid_pairing_consume_approval(&state, 17));
    assert(!hid_pairing_consume_approval(&state, 17));
    assert(hid_pairing_add_bond(&state, peer(1), 0x1234));
    assert(hid_pairing_token(&state, peer(1)) == 0x1234);
    hid_pairing_cancel(&state);
    assert(hid_pairing_admit(&state, peer(1), 80000));
    assert(!hid_pairing_admit(&state, peer(2), 80000));
    for (unsigned index = 2; index <= HID_PAIRING_MAX_BONDS; ++index)
        assert(hid_pairing_add_bond(&state, peer(index), 0x1234 + index));
    assert(!hid_pairing_open(&state, 90000));
    assert(!hid_pairing_add_bond(&state, peer(9), 9999));
    assert(!hid_pairing_forget(&state, 0x1234, false));
    assert(hid_pairing_token(&state, peer(1)) == 0x1234);
    assert(hid_pairing_forget(&state, 0x1234, true));
    assert(!hid_pairing_admit(&state, peer(1), 90000));
    assert(hid_pairing_open(&state, 90000));
    assert(hid_pairing_begin_challenge(&state, peer(9), 19, 654321, 90000));
    assert(!hid_pairing_confirm(&state, 19, true, 150000));
    assert(!hid_pairing_consume_approval(&state, 19));
    assert(!hid_pairing_challenge_pending(&state));
    hid_pairing_t reloaded = state;
    assert(hid_pairing_token(&reloaded, peer(2)) == 0x1236);
    return 0;
}
