/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies that a guest-initiated numeric comparison becomes a bounded,
 * device-approved challenge while the Mac remains responsible for user consent.
 * Reuses the existing NimBLE host mocks without device access. */
#define main legacy_hid_guest_test
#include "test_hid_guest.c"
#undef main

int main(void)
{
    active_peer.type = 1;
    active_peer.val[0] = 42;
    assert(hid_guest_start() == 0);
    hid_guest_pairing_set_events(event_sink, NULL);
    ble_hs_cfg.sync_cb();

    struct ble_gap_event connect = {.type = BLE_GAP_EVENT_CONNECT};
    connect.connect.conn_handle = 17;
    gap_callback(&connect, NULL);
    assert(channel.connected);

    struct ble_gap_event challenge = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    challenge.passkey.conn_handle = 17;
    challenge.passkey.params.action = BLE_SM_IOACT_NUMCMP;
    challenge.passkey.params.numcmp = 123456;
    gap_callback(&challenge, NULL);
    assert(last_event.type == HID_GUEST_PAIRING_CHALLENGE);
    assert(last_event.number == 123456);
    assert(confirmations == 1);

    hid_pairing_t snapshot;
    hid_guest_pairing_snapshot(&snapshot);
    assert(snapshot.challenge_approved && !snapshot.challenge_active);
    struct ble_gap_event encryption = {.type = BLE_GAP_EVENT_ENC_CHANGE};
    encryption.enc_change.conn_handle = 17;
    gap_callback(&encryption, NULL);
    assert(last_event.type == HID_GUEST_BONDED);
    assert(channel.encrypted && saved_size);

    struct ble_gap_event unsupported = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    unsupported.passkey.conn_handle = 18;
    unsupported.passkey.params.action = 0xff;
    unsigned accepted_before = confirmations;
    gap_callback(&unsupported, NULL);
    assert(last_event.type == HID_GUEST_PAIRING_REJECTED);
    assert(confirmations == accepted_before);

    active_peer.val[0] = 99;
    struct ble_gap_event second_connect = {.type = BLE_GAP_EVENT_CONNECT};
    second_connect.connect.conn_handle = 18;
    gap_callback(&second_connect, NULL);
    challenge.passkey.conn_handle = 18;
    challenge.passkey.params.numcmp = 654321;
    gap_callback(&challenge, NULL);
    assert(last_event.type == HID_GUEST_PAIRING_CHALLENGE && confirmations == accepted_before + 1);
    struct ble_gap_event disconnect = {.type = BLE_GAP_EVENT_DISCONNECT};
    disconnect.disconnect.conn.conn_handle = 18;
    gap_callback(&disconnect, NULL);
    assert(last_event.type == HID_GUEST_PAIRING_REJECTED);

    hid_pairing_t limited;
    hid_pairing_init(&limited);
    hid_peer_t another = {.type = 1, .address = {9}};
    assert(hid_pairing_open(&limited, 1000));
    for (uint32_t attempt = 1; attempt <= HID_PAIRING_MAX_ATTEMPTS; ++attempt) {
        assert(hid_pairing_begin_challenge(&limited, another, 18, attempt,
                                           100000 + attempt, 1000 + attempt));
        assert(!hid_pairing_confirm(&limited, attempt, false, 1000 + attempt));
    }
    assert(!hid_pairing_begin_challenge(&limited, another, 18, 99, 123456, 2000));
    assert(hid_pairing_open(&limited, 61001));
    assert(hid_pairing_begin_challenge(&limited, another, 18, 100, 123456, 61002));
    hid_pairing_t late;
    hid_pairing_init(&late);
    assert(hid_pairing_open(&late, 1000));
    assert(hid_pairing_begin_challenge(&late, another, 18, 7, 654321, 60900));
    assert(late.deadline_ms == 60900 + HID_PAIRING_CHALLENGE_MIN_MS);
    assert(hid_pairing_confirm(&late, 7, true, 80000));
    return 0;
}
