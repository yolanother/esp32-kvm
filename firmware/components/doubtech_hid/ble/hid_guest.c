/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Starts a bounded NimBLE HID peripheral after explicit firmware integration.
 * It admits up to three isolated connections to one composite HID service, requires an
 * authenticated bonded link and guest-side numeric consent, while routing stays
 * disarmed until the host actor arms. A connected peer's opaque token is
 * resolved from the persisted pairing table only after link authentication.
 * Retained inventory copies opaque tokens on the NimBLE host loop. Only the
 * first channel is routed pending a multi-slot host/status contract. */
#include "hid_guest.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "hid_gatt.h"
#include "hid_pairing_store.h"
#include "hid_guest_rpc.h"

/* ESP-IDF's NimBLE store configuration helper has no public header. */
void ble_store_config_init(void);

static const char *tag = "esp32-kvm-hid";
static uint8_t own_address_type;
static hid_pairing_t pairing;
static hid_guest_pairing_event_fn pairing_events;
static void *pairing_event_context;
static bool started;
static struct ble_npl_event disconnect_events[HID_GATT_MAX_CONNECTIONS];
static struct ble_npl_callout pairing_timeout;
static struct ble_npl_callout advertising_retry;
static size_t reconnect_phase;

#define HID_ADVERTISING_RETRY_MS 250u
#define HID_RECONNECT_PHASE_MS 500u

static void schedule_advertising_retry(void);

static void disconnect_on_host(struct ble_npl_event *event)
{
    for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot)
        if (event == &disconnect_events[slot - 1]) {
            (void)hid_guest_disconnect_slot(slot);
            return;
        }
}

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void publish(hid_guest_pairing_event_type_t type, uint16_t handle,
                    uint32_t number, const hid_token_t *token)
{
    if (!pairing_events) return;
    hid_guest_pairing_event_t event = {.type = type, .connection_handle = handle,
                                      .number = number, .deadline_ms = pairing.deadline_ms};
    if (type == HID_GUEST_PAIRING_CHALLENGE) event.challenge_id = pairing.challenge_id;
    if (token) event.token = *token;
    pairing_events(&event, pairing_event_context);
}

static void expire_window(void)
{
    bool was_open = pairing.window_active;
    bool pending = pairing.challenge_active || pairing.challenge_approved;
    uint16_t handle = pairing.challenge_handle;
    if (was_open && !hid_pairing_window_open(&pairing, now_ms())) {
        if (pending) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        publish(HID_GUEST_PAIRING_TIMEOUT, 0, 0, NULL);
    }
}

static void pairing_timeout_on_host(struct ble_npl_event *event)
{
    (void)event;
    bool was_open = pairing.window_active;
    expire_window();
    if (was_open && !pairing.window_active) {
        (void)ble_gap_adv_stop();
        schedule_advertising_retry();
    }
}

static hid_peer_t peer_identity(const ble_addr_t *address)
{
    hid_peer_t peer = {.type = address->type};
    memcpy(peer.address, address->val, sizeof(peer.address));
    return peer;
}

static ble_addr_t ble_identity(hid_peer_t peer)
{
    ble_addr_t address = {.type = peer.type};
    memcpy(address.val, peer.address, sizeof(peer.address));
    return address;
}

/* NimBLE's sample round-robin callback evicts a bond. Nonzero forbids retry. */
static int reject_store_overflow(struct ble_store_status_event *event, void *argument)
{
    (void)event; (void)argument;
    ESP_LOGE(tag, "Bond store full; no identity evicted");
    return 1;
}

static int gap_event(struct ble_gap_event *event, void *argument);
static void advertise(void);

static void advertising_retry_on_host(struct ble_npl_event *event)
{
    (void)event;
    advertise();
}

static void schedule_advertising_retry(void)
{
    if (ble_npl_callout_reset(&advertising_retry,
            ble_npl_time_ms_to_ticks32(HID_ADVERTISING_RETRY_MS)) != BLE_NPL_OK)
        ESP_LOGE(tag, "Could not schedule HID advertising retry");
}

static bool peer_is_connected(hid_peer_t peer)
{
    for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
        hid_channel_t *channel = hid_gatt_channel_at(slot);
        struct ble_gap_conn_desc description;
        if (channel->connected &&
            ble_gap_conn_find(channel->connection_handle, &description) == 0 &&
            description.peer_id_addr.type == peer.type &&
            memcmp(description.peer_id_addr.val, peer.address, sizeof(peer.address)) == 0)
            return true;
    }
    return false;
}

/* Each boot or disconnect starts one short directed sweep over the retained
   peers. Once it is exhausted the device stays on continuous undirected
   advertising: a central that reconnects with a private address never matches a
   directed target, so a repeating sweep would leave it a fraction of the time. */
static void restart_reconnect_sweep(void)
{
    reconnect_phase = 0;
}

static bool next_reconnect_peer(ble_addr_t *address)
{
    if (!address || pairing.window_active || pairing.bond_count == 0) return false;
    while (reconnect_phase < pairing.bond_count) {
        hid_peer_t peer = pairing.bonds[reconnect_phase++].peer;
        if (!peer_is_connected(peer)) {
            *address = ble_identity(peer);
            return true;
        }
    }
    return false;
}

static void advertise(void)
{
    static const ble_uuid16_t service[] = {BLE_UUID16_INIT(0x1812)};
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    static const char name[] = "ESP32 KVM KB Test";
#else
    static const char name[] = "ESP32 KVM";
#endif
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params parameters = {0};
    ble_addr_t reconnect_address;
    if (hid_gatt_connection_count() >= HID_GATT_MAX_CONNECTIONS) {
        ble_npl_callout_stop(&advertising_retry);
        return;
    }
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = sizeof(name) - 1;
    fields.name_is_complete = 1;
    fields.uuids16 = service;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = 0x03c1; /* Keyboard is the primary HID function. */
    fields.appearance_is_present = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(tag, "HID advertising fields rejected");
        schedule_advertising_retry();
        return;
    }
    bool directed = next_reconnect_peer(&reconnect_address);
    parameters.conn_mode = directed ? BLE_GAP_CONN_MODE_DIR : BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = directed ? BLE_GAP_DISC_MODE_NON : BLE_GAP_DISC_MODE_GEN;
    /* Apple recommends an exact 20 ms interval for prompt HID discovery. */
    parameters.itvl_min = 32;
    parameters.itvl_max = 32;
    uint32_t duration = directed ? HID_RECONNECT_PHASE_MS : BLE_HS_FOREVER;
    int result = ble_gap_adv_start(own_address_type,
                                   directed ? &reconnect_address : NULL, duration,
                                   &parameters, gap_event, NULL);
    if (result == 0 || result == BLE_HS_EALREADY) {
        ble_npl_callout_stop(&advertising_retry);
        return;
    }
    ESP_LOGW(tag, "HID advertising deferred after transient error: %d", result);
    schedule_advertising_retry();
}

static void on_reset(int reason)
{
    (void)reason;
    ble_npl_callout_stop(&advertising_retry);
    for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
        hid_channel_t *channel = hid_gatt_channel_at(slot);
        if (channel->connected) hid_channel_disconnected(channel);
    }
    publish(HID_GUEST_DISCONNECTED, 0, 0, NULL);
}

static void on_sync(void)
{
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    /* Use a separate static identity for the diagnostic profile. macOS keeps
       GATT data by peripheral identity even after the Bluetooth UI forgets a
       device, so changing only the name cannot force fresh HID discovery. */
    static const uint8_t diagnostic_address[6] = {
        0x51, 0x32, 0x84, 0x76, 0x5a, 0xe1
    };
    if (ble_hs_id_set_rnd(diagnostic_address) != 0) {
        ESP_LOGE(tag, "Could not set diagnostic BLE identity");
        return;
    }
    own_address_type = BLE_OWN_ADDR_RANDOM;
#else
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &own_address_type) != 0) {
        ESP_LOGE(tag, "No stable BLE identity available");
        return;
    }
#endif
    /* The HID report set changed while paired development Macs retained their
       bonds. Mark the full database changed on every boot so bonded centrals
       invalidate cached report handles before input notifications resume. */
    ble_svc_gatt_changed(0x0001, 0xffff);
    restart_reconnect_sweep();
    advertise();
}

/* Encryption and CCCD writes can be reported for a link before this callback has
   seen its CONNECT event (a bonded Mac secures the link immediately). Register
   the live link on first sight so those events are not discarded; CONNECT then
   finds it already present. Events for a link NimBLE already removed are ignored. */
static hid_channel_t *live_channel(uint16_t connection_handle)
{
    hid_channel_t *channel = hid_gatt_channel_for(connection_handle);
    struct ble_gap_conn_desc description;
    if (channel || ble_gap_conn_find(connection_handle, &description) != 0) return channel;
    if (!hid_gatt_on_connect(connection_handle)) return NULL;
    ESP_LOGW(tag, "BLE event preceded CONNECT; registered handle %u", connection_handle);
    return hid_gatt_channel_for(connection_handle);
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(tag, "BLE connection attempt failed: %d", event->connect.status);
            advertise();
            return 0;
        }
        expire_window();
        /* A bonded central may reconnect using a private address.  Its stored
           identity is not trustworthy until link security has completed; the
           ENC_CHANGE path checks the authenticated bond before exposing a slot. */
        ESP_LOGI(tag, "BLE link opened; authenticating handle %u", event->connect.conn_handle);
        if (!hid_gatt_channel_for(event->connect.conn_handle) &&
            !hid_gatt_on_connect(event->connect.conn_handle)) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
        /* Match a normal HID peripheral: let the central start security when
         * reading encrypted report references. Request it only as a fallback
         * when the central subscribes to input before securing the link. */
        advertise();
#else
        /* A bonded central that already secured the link needs no Security Request;
           an unanswered one would time out after 30 s and drop a working link. */
        hid_channel_t *opened = hid_gatt_channel_for(event->connect.conn_handle);
        int security_result = opened && opened->encrypted ?
                              0 : ble_gap_security_initiate(event->connect.conn_handle);
        /* The central may have begun SMP first.  An existing procedure is
           still allowed to finish through ENC_CHANGE; only a real failure
           invalidates the link. */
        if (security_result != 0 && security_result != BLE_HS_EALREADY) {
            ESP_LOGW(tag, "BLE security start failed: %d", security_result);
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else advertise();
#endif
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(tag, "BLE link closed: reason %d", event->disconnect.reason);
        uint8_t disconnected_slot = 0;
        for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
            hid_channel_t *channel = hid_gatt_channel_at(slot);
            if (channel->connected &&
                channel->connection_handle == event->disconnect.conn.conn_handle)
                disconnected_slot = slot;
        }
        hid_gatt_on_disconnect(event->disconnect.conn.conn_handle);
        if (disconnected_slot)
            publish(HID_GUEST_DISCONNECTED, event->disconnect.conn.conn_handle,
                    disconnected_slot, NULL);
        if ((pairing.challenge_active || pairing.challenge_approved) &&
            pairing.challenge_handle == event->disconnect.conn.conn_handle)
            publish(HID_GUEST_PAIRING_REJECTED, event->disconnect.conn.conn_handle, 0, NULL);
        if (pairing.challenge_handle == event->disconnect.conn.conn_handle) {
            pairing.challenge_active = false;
            pairing.challenge_approved = false;
            pairing.challenge_id = 0;
        }
        restart_reconnect_sweep();
        advertise();
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        hid_channel_t *known = live_channel(event->enc_change.conn_handle);
        if (event->enc_change.status != 0 && known && known->encrypted) {
            /* A late procedure failure must not drop a link that is already secure. */
            ESP_LOGW(tag, "Ignoring security status %d on authenticated handle %u",
                     event->enc_change.status, event->enc_change.conn_handle);
            return 0;
        }
        struct ble_gap_conn_desc description;
        bool secure = event->enc_change.status == 0 &&
                      ble_gap_conn_find(event->enc_change.conn_handle, &description) == 0 &&
                      description.sec_state.encrypted && description.sec_state.bonded &&
                      description.sec_state.authenticated;
        if (secure) {
            hid_peer_t peer = peer_identity(&description.peer_id_addr);
            hid_token_t existing;
            if (!hid_pairing_token(&pairing, peer, &existing)) {
                expire_window();
                secure = pairing.window_active && pairing.bond_count < HID_PAIRING_MAX_BONDS &&
                         hid_pairing_consume_approval(&pairing, event->enc_change.conn_handle);
                if (secure) {
                    hid_token_t token;
                    for (size_t index = 0; index < HID_PAIRING_TOKEN_LEN; index += 4) {
                        uint32_t random = esp_random();
                        memcpy(&token.bytes[index], &random, sizeof(random));
                    }
                    hid_pairing_t next = pairing;
                    secure = hid_pairing_add_bond(&next, peer, token) &&
                             hid_pairing_store_save(&next) == ESP_OK;
                    if (secure) {
                        pairing = next;
                        ble_npl_callout_stop(&pairing_timeout);
                        publish(HID_GUEST_BONDED, event->enc_change.conn_handle, 0, &token);
                    } else {
                        ble_addr_t address = description.peer_id_addr;
                        ble_store_util_delete_peer(&address);
                    }
                }
            }
        }
        hid_gatt_on_encryption(event->enc_change.conn_handle, secure);
        if (!secure) {
            ESP_LOGW(tag, "BLE authentication rejected: status %d", event->enc_change.status);
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else ESP_LOGI(tag, "BLE authenticated handle %u", event->enc_change.conn_handle);
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
#ifdef CONFIG_KVM_HID_GATT_TRACE
        ESP_LOGI(tag, "HID subscribe conn=%u handle=%u notify=%u",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
#endif
        (void)live_channel(event->subscribe.conn_handle);
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
        hid_channel_t *channel = hid_gatt_channel_for(event->subscribe.conn_handle);
        bool had_keyboard_subscription = channel &&
            (channel->subscribed[HID_REPORT_KEYBOARD] || channel->subscribed[HID_REPORT_BOOT_KEYBOARD]);
#endif
        hid_gatt_on_subscribe(event->subscribe.conn_handle,
                              event->subscribe.attr_handle, event->subscribe.cur_notify);
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
        if (event->subscribe.cur_notify && channel && !channel->encrypted && !had_keyboard_subscription &&
            (channel->subscribed[HID_REPORT_KEYBOARD] ||
             channel->subscribed[HID_REPORT_BOOT_KEYBOARD])) {
            int result = ble_gap_security_initiate(event->subscribe.conn_handle);
            if (result != 0 && result != BLE_HS_EALREADY) {
                ESP_LOGW(tag, "HID subscription security failed: %d", result);
                ble_gap_terminate(event->subscribe.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
#endif
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_TX: {
#ifdef CONFIG_KVM_HID_GATT_TRACE
        ESP_LOGI(tag, "HID notify conn=%u handle=%u status=%d",
                 event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                 event->notify_tx.status);
#endif
        hid_channel_t *channel = hid_gatt_channel_for(event->notify_tx.conn_handle);
        if (channel) {
            if (event->notify_tx.status == 0) {
                channel->notify_tx_successes++;
            } else {
                channel->armed = false;
                channel->needs_disconnect = true;
                ble_gap_terminate(channel->connection_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        expire_window();
        advertise();
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_gap_conn_desc description;
        uint16_t handle = event->passkey.conn_handle;
        expire_window();
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP &&
            !pairing.window_active && hid_guest_pairing_open() != ESP_OK) {
            publish(HID_GUEST_PAIRING_REJECTED, handle, 0, NULL);
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint32_t challenge_id;
        do { challenge_id = esp_random(); }
        while (challenge_id == 0 || challenge_id == pairing.last_challenge_id);
        if (event->passkey.params.action != BLE_SM_IOACT_NUMCMP ||
            ble_gap_conn_find(handle, &description) != 0 ||
            !hid_pairing_begin_challenge(&pairing, peer_identity(&description.peer_id_addr),
                                         handle, challenge_id, event->passkey.params.numcmp, now_ms())) {
            publish(HID_GUEST_PAIRING_REJECTED, handle, 0, NULL);
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint64_t remaining_ms = pairing.deadline_ms - now_ms();
        if (ble_npl_callout_reset(&pairing_timeout,
                ble_npl_time_ms_to_ticks32((uint32_t)remaining_ms)) != BLE_NPL_OK) {
            hid_pairing_cancel(&pairing);
            publish(HID_GUEST_PAIRING_REJECTED, handle, 0, NULL);
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        publish(HID_GUEST_PAIRING_CHALLENGE, handle, pairing.challenge_number, NULL);
        /* The guest confirms the displayed code on its own screen. NimBLE may
           complete only after that peer's confirmation and authenticated bonding. */
        if (hid_guest_pairing_confirm(challenge_id, true) != ESP_OK) {
            pairing.challenge_approved = false;
            publish(HID_GUEST_PAIRING_REJECTED, handle, 0, NULL);
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* A peer that forgot its key can repair only during an explicitly
           opened pairing window. Keep every other peer's bond untouched. */
        uint16_t handle = event->repeat_pairing.conn_handle;
        struct ble_gap_conn_desc description;
        expire_window();
        bool allowed = pairing.window_active &&
                       event->repeat_pairing.new_authenticated &&
                       event->repeat_pairing.new_sc &&
                       event->repeat_pairing.new_bonding &&
                       ble_gap_conn_find(handle, &description) == 0;
        if (allowed) {
            hid_peer_t peer = peer_identity(&description.peer_id_addr);
            for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
                hid_channel_t *other = hid_gatt_channel_at(slot);
                struct ble_gap_conn_desc other_description;
                if (!other->connected || other->connection_handle == handle) continue;
                if (ble_gap_conn_find(other->connection_handle, &other_description) == 0 &&
                    other_description.peer_id_addr.type == description.peer_id_addr.type &&
                    memcmp(other_description.peer_id_addr.val, description.peer_id_addr.val,
                           sizeof(description.peer_id_addr.val)) == 0) allowed = false;
            }
            if (allowed) {
                hid_pairing_t next = pairing;
                hid_token_t old_token;
                bool known = hid_pairing_token(&pairing, peer, &old_token);
                if (known && (!hid_pairing_forget(&next, old_token, true) ||
                              hid_pairing_store_save(&next) != ESP_OK)) allowed = false;
                if (allowed && ble_store_util_delete_peer(&description.peer_id_addr) == 0) {
                    pairing = next;
                    ESP_LOGI(tag, "Replacing one forgotten BLE bond during pairing window");
                    return BLE_GAP_REPEAT_PAIRING_RETRY;
                }
                if (known && allowed) (void)hid_pairing_store_save(&pairing);
            }
        }
        ESP_LOGW(tag, "Repeat pairing rejected outside safe repair window");
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    default: return 0;
    }
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t hid_guest_start(void)
{
    if (started) return ESP_ERR_INVALID_STATE;
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result; /* Never erase stored bonds implicitly. */
    result = hid_pairing_store_load(&pairing);
    if (result != ESP_OK) return result;
    result = nimble_port_init();
    if (result != ESP_OK) return result;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = reject_store_overflow;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = hid_gatt_register();
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
#ifdef CONFIG_KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC
    result = ble_svc_gap_device_name_set("ESP32 KVM KB Test");
#else
    result = ble_svc_gap_device_name_set("ESP32 KVM");
#endif
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    result = ble_svc_gap_device_appearance_set(0x03c1);
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    ble_store_config_init();
    result = hid_guest_rpc_init();
    if (result != ESP_OK) { nimble_port_deinit(); return result; }
    for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot)
        ble_npl_event_init(&disconnect_events[slot - 1], disconnect_on_host, NULL);
    ble_npl_callout_init(&pairing_timeout, nimble_port_get_dflt_eventq(),
                         pairing_timeout_on_host, NULL);
    ble_npl_callout_init(&advertising_retry, nimble_port_get_dflt_eventq(),
                         advertising_retry_on_host, NULL);
    nimble_port_freertos_init(host_task);
    started = true;
    return ESP_OK;
}

void hid_guest_pairing_set_events(hid_guest_pairing_event_fn callback, void *context)
{
    pairing_events = callback;
    pairing_event_context = context;
}

esp_err_t hid_guest_pairing_open(void)
{
    if (!started || !pairing_events) return ESP_ERR_INVALID_STATE;
    ble_addr_t bonds[HID_PAIRING_MAX_BONDS];
    int count = 0;
    if (ble_store_util_bonded_peers(bonds, &count, HID_PAIRING_MAX_BONDS) != 0)
        return ESP_ERR_INVALID_STATE;
    if (count >= HID_PAIRING_MAX_BONDS || pairing.bond_count >= HID_PAIRING_MAX_BONDS) {
        publish(HID_GUEST_PAIRING_CAPACITY, 0, 0, NULL);
        return ESP_ERR_INVALID_STATE;
    }
    if (!hid_pairing_open(&pairing, now_ms())) return ESP_ERR_INVALID_STATE;
    if (ble_npl_callout_reset(&pairing_timeout,
                             ble_npl_time_ms_to_ticks32(HID_PAIRING_WINDOW_MS)) != BLE_NPL_OK) {
        hid_pairing_cancel(&pairing);
        return ESP_FAIL;
    }
    publish(HID_GUEST_PAIRING_OPENED, 0, 0, NULL);
    (void)ble_gap_adv_stop();
    schedule_advertising_retry();
    return ESP_OK;
}

void hid_guest_pairing_cancel(void)
{
    bool pending = pairing.challenge_active || pairing.challenge_approved;
    uint16_t handle = pairing.challenge_handle;
    hid_pairing_cancel(&pairing);
    ble_npl_callout_stop(&pairing_timeout);
    if (pending) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    publish(HID_GUEST_PAIRING_CLOSED, 0, 0, NULL);
    (void)ble_gap_adv_stop();
    schedule_advertising_retry();
}

esp_err_t hid_guest_pairing_confirm(uint32_t challenge_id, bool approved)
{
    expire_window();
    if (!pairing.challenge_active || !challenge_id || pairing.challenge_id != challenge_id)
        return ESP_ERR_INVALID_STATE;
    uint16_t connection_handle = pairing.challenge_handle;
    bool accepted = hid_pairing_confirm(&pairing, challenge_id, approved, now_ms());
    struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = accepted};
    if (ble_sm_inject_io(connection_handle, &io) != 0) return ESP_FAIL;
    if (!accepted) {
        publish(HID_GUEST_PAIRING_REJECTED, connection_handle, 0, NULL);
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}

void hid_guest_pairing_snapshot(hid_pairing_t *output)
{
    expire_window();
    *output = pairing;
}

static bool bond_token_at(uint8_t slot, hid_token_t *output)
{
    if (!output) return false;
    memset(output, 0, sizeof(*output));
    hid_channel_t *channel = hid_gatt_channel_at(slot);
    if (!channel) return false;
    if (!started || !channel->connected || !channel->encrypted || channel->needs_disconnect)
        return false;
    struct ble_gap_conn_desc description;
    if (ble_gap_conn_find(channel->connection_handle, &description) != 0 ||
        description.conn_handle != channel->connection_handle)
        return false;
    bool authenticated = description.sec_state.encrypted &&
                         description.sec_state.bonded &&
                         description.sec_state.authenticated;
    return hid_pairing_connected_token(&pairing, true, authenticated,
                                       peer_identity(&description.peer_id_addr), output);
}

bool hid_guest_current_bond_token(hid_token_t *output)
{ return bond_token_at(1, output); }

bool hid_guest_snapshot_slots(hid_guest_slot_snapshot_t output[HID_GATT_MAX_CONNECTIONS])
{
    if (!output) return false;
    memset(output, 0, HID_GATT_MAX_CONNECTIONS * sizeof(*output));
    if (!started) return false;
    for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
        hid_channel_t *channel = hid_gatt_channel_at(slot);
        hid_token_t token;
        if (!bond_token_at(slot, &token)) {
            if (channel->armed) (void)hid_guest_disconnect_slot(slot);
            continue;
        }
        hid_guest_slot_snapshot_t *item = &output[slot - 1];
        memcpy(item->token, token.bytes, sizeof(item->token));
        item->ready = true;
        item->subscribed = hid_channel_ready(channel);
        item->notify_tx_successes = channel->notify_tx_successes;
    }
    return true;
}

bool hid_guest_retained_bonds(hid_token_t output[HID_PAIRING_MAX_BONDS], size_t *count)
{
    if (!output || !count) return false;
    *count = 0;
    memset(output, 0, HID_PAIRING_MAX_BONDS * sizeof(*output));
    return started && hid_pairing_inventory(&pairing, output, count);
}

esp_err_t hid_guest_pairing_forget(hid_token_t token, bool confirmed)
{
    static const hid_token_t zero = {{0}};
    if (!started || !confirmed || memcmp(token.bytes, zero.bytes, HID_PAIRING_TOKEN_LEN) == 0)
        return ESP_ERR_INVALID_ARG;
    hid_pairing_t next = pairing;
    hid_peer_t peer = {0};
    bool found = false;
    for (size_t index = 0; index < next.bond_count; ++index)
        if (memcmp(next.bonds[index].token.bytes, token.bytes, HID_PAIRING_TOKEN_LEN) == 0)
            { peer = next.bonds[index].peer; found = true; break; }
    if (!found || !hid_pairing_forget(&next, token, true)) return ESP_ERR_NOT_FOUND;
    ble_addr_t address = ble_identity(peer);
    if (ble_store_util_delete_peer(&address) != 0) return ESP_FAIL;
    esp_err_t result = hid_pairing_store_save(&next);
    if (result == ESP_OK) {
        pairing = next;
        for (uint8_t slot = 1; slot <= HID_GATT_MAX_CONNECTIONS; ++slot) {
            hid_channel_t *channel = hid_gatt_channel_at(slot);
            struct ble_gap_conn_desc description;
            if (!channel->connected) continue;
            if (ble_gap_conn_find(channel->connection_handle, &description) != 0) {
                channel->armed = false;
                channel->needs_disconnect = true;
                ble_gap_terminate(channel->connection_handle, BLE_ERR_REM_USER_CONN_TERM);
                continue;
            }
            hid_peer_t connected = peer_identity(&description.peer_id_addr);
            if (connected.type != peer.type ||
                memcmp(connected.address, peer.address, sizeof(peer.address)) != 0)
                continue;
            channel->armed = false;
            channel->needs_disconnect = true;
            ble_gap_terminate(channel->connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return result;
}

esp_err_t hid_guest_disconnect_current(void)
{ return hid_guest_disconnect_slot(1); }

esp_err_t hid_guest_disconnect_slot(uint8_t slot)
{
    hid_channel_t *channel = hid_gatt_channel_at(slot);
    if (!channel) return ESP_ERR_INVALID_ARG;
    channel->armed = false;
    channel->needs_disconnect = true;
    if (!channel->connected) return ESP_OK;
    return ble_gap_terminate(channel->connection_handle,
                             BLE_ERR_REM_USER_CONN_TERM) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t hid_guest_request_disconnect(void)
{ return hid_guest_request_disconnect_slot(1); }

esp_err_t hid_guest_request_disconnect_slot(uint8_t slot)
{
    if (!started) return ESP_ERR_INVALID_STATE;
    if (!slot || slot > HID_GATT_MAX_CONNECTIONS) return ESP_ERR_INVALID_ARG;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &disconnect_events[slot - 1]);
    return ESP_OK;
}
