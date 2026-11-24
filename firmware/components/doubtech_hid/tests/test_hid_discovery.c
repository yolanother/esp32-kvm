/* Check the keyboard diagnostic defers security until HID discovery needs it. */
#define main legacy_doubtech_hid_test
#include "test_hid_guest.c"
#undef main
int main(void)
{
    active_peer.type = 1; active_peer.val[0] = 42;
    assert(doubtech_hid_start() == 0);
    doubtech_hid_pairing_set_events(event_sink, NULL);
    ble_hs_cfg.sync_cb();
    struct ble_gap_event connect = {.type = BLE_GAP_EVENT_CONNECT};
    connect.connect.conn_handle = 17;
    gap_callback(&connect, NULL);
    assert(channel.connected && !channel.encrypted && !security_requests);
    struct ble_gap_event subscription = {.type = BLE_GAP_EVENT_SUBSCRIBE};
    subscription.subscribe.conn_handle = 17;
    subscription.subscribe.attr_handle = 37; /* Battery is not HID input. */
    subscription.subscribe.cur_notify = 1;
    gap_callback(&subscription, NULL);
    assert(!security_requests && !channel.subscribed[HID_REPORT_KEYBOARD]);
    security_result = BLE_HS_EALREADY; /* Mac may already have begun SMP. */
    subscription.subscribe.attr_handle = 10;
    gap_callback(&subscription, NULL);
    assert(security_requests == 1 && !terminations && !hid_channel_ready(&channel));
    subscription.subscribe.attr_handle = 37;
    gap_callback(&subscription, NULL);
    assert(security_requests == 1); /* No duplicate request for another service. */
    struct ble_gap_event challenge = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    challenge.passkey.conn_handle = 17;
    challenge.passkey.params.action = BLE_SM_IOACT_NUMCMP;
    challenge.passkey.params.numcmp = 123456;
    gap_callback(&challenge, NULL);
    assert(last_event.type == DOUBTECH_HID_PAIRING_CHALLENGE && confirmations == 1);
    struct ble_gap_event encryption = {.type = BLE_GAP_EVENT_ENC_CHANGE};
    encryption.enc_change.conn_handle = 17;
    gap_callback(&encryption, NULL);
    assert(channel.encrypted && hid_channel_ready(&channel) && saved_size);
    assert(last_event.type == DOUBTECH_HID_BONDED);
    subscription.subscribe.attr_handle = 10;
    gap_callback(&subscription, NULL);
    assert(security_requests == 1);
    struct ble_gap_event disconnect = {.type = BLE_GAP_EVENT_DISCONNECT};
    disconnect.disconnect.conn.conn_handle = 17;
    gap_callback(&disconnect, NULL);
    assert(!channel.connected && !hid_channel_ready(&channel));
    return 0;
}
