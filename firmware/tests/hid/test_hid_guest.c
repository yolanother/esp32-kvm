/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Exercises HID guest admission, numeric consent, authenticated bonding,
 * token persistence, bounded current-token lookup, disconnect clearing,
 * retained inventory, and deliberate removal with mocked NimBLE and NVS. */
#include <assert.h>
#include <string.h>
#include "hid_guest.h"
#include "hid_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nvs.h"
#include "nimble/nimble_npl.h"

struct ble_hs_cfg ble_hs_cfg;
static hid_channel_t channel;
static hid_channel_t other_channels[HID_GATT_MAX_CONNECTIONS - 1];
static uint16_t last_report_handle;
static int (*gap_callback)(struct ble_gap_event *, void *);
static ble_addr_t active_peer;
static int64_t time_us;
static unsigned terminations, advertisements, confirmations;
static unsigned char saved[256];
static size_t saved_size;
static hid_guest_pairing_event_t last_event;
static unsigned sent_reports;
static struct ble_npl_eventq host_queue;
static struct ble_npl_callout host_timeout;
static bool hold_host;
static bool fail_next_commit;

static void event_sink(const hid_guest_pairing_event_t *event, void *context)
{ (void)context; last_event = *event; }
static int send_report(void *context, uint16_t handle, uint8_t report,
                       const uint8_t *bytes, size_t length)
{
    (void)context; (void)bytes;
    assert((handle == 17 || handle == 18) && report >= HID_REPORT_KEYBOARD && report <= HID_REPORT_CONSUMER);
    last_report_handle = handle;
    assert(length == (report == HID_REPORT_KEYBOARD ? HID_KEYBOARD_REPORT_LEN :
                      report == HID_REPORT_MOUSE ? HID_MOUSE_REPORT_LEN : HID_CONSUMER_REPORT_LEN));
    sent_reports++;
    return 0;
}
int nvs_flash_init(void) { return 0; }
int nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)mode; assert(strcmp(name, "kvm_bonds") == 0); *handle = 1; return 0; }
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{ (void)handle; (void)key; if (!saved_size) return ESP_ERR_NVS_NOT_FOUND;
  assert(*length >= saved_size); memcpy(value, saved, saved_size); *length = saved_size; return 0; }
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{ (void)handle; (void)key; assert(length <= sizeof(saved));
  memcpy(saved, value, length); saved_size = length; return 0; }
int nvs_commit(nvs_handle_t handle)
{ (void)handle; if (fail_next_commit) { fail_next_commit = false; return ESP_FAIL; } return 0; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
uint32_t esp_random(void) { static uint32_t next = 0x1234; return ++next; }
int64_t esp_timer_get_time(void) { return time_us; }
int nimble_port_init(void) { return 0; }
int nimble_port_deinit(void) { return 0; }
void nimble_port_run(void) { }
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &host_queue; }
void ble_npl_event_init(struct ble_npl_event *event,
                        void (*callback)(struct ble_npl_event *), void *argument)
{ event->callback = callback; event->argument = argument; }
void ble_npl_eventq_put(struct ble_npl_eventq *queue, struct ble_npl_event *event)
{ if (!queue->pending) queue->pending = event; else queue->next = event; }
void ble_npl_callout_init(struct ble_npl_callout *callout, struct ble_npl_eventq *queue,
                          void (*callback)(struct ble_npl_event *), void *argument)
{ (void)queue; (void)argument; callout->callback = callback; host_timeout = *callout; }
int ble_npl_callout_reset(struct ble_npl_callout *callout, uint32_t ticks)
{ assert(callout->callback && ticks == 60000); return BLE_NPL_OK; }
void ble_npl_callout_stop(struct ble_npl_callout *callout) { (void)callout; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
ble_npl_time_t ble_npl_time_get(void) { return (ble_npl_time_t)(time_us / 1000); }
int ble_npl_mutex_init(struct ble_npl_mutex *mutex) { (void)mutex; return BLE_NPL_OK; }
int ble_npl_mutex_pend(struct ble_npl_mutex *mutex, ble_npl_time_t timeout)
{ (void)mutex; (void)timeout; return BLE_NPL_OK; }
int ble_npl_mutex_release(struct ble_npl_mutex *mutex) { (void)mutex; return BLE_NPL_OK; }
int ble_npl_sem_init(struct ble_npl_sem *semaphore, uint16_t tokens)
{ semaphore->tokens = tokens; return BLE_NPL_OK; }
int ble_npl_sem_pend(struct ble_npl_sem *semaphore, ble_npl_time_t timeout)
{
    if (semaphore->tokens) { semaphore->tokens--; return BLE_NPL_OK; }
    if (timeout && host_queue.pending && !hold_host) {
        struct ble_npl_event *event = host_queue.pending;
        host_queue.pending = host_queue.next;
        host_queue.next = NULL;
        event->callback(event);
        if (semaphore->tokens) { semaphore->tokens--; return BLE_NPL_OK; }
    }
    return 1;
}
int ble_npl_sem_release(struct ble_npl_sem *semaphore)
{ semaphore->tokens++; return BLE_NPL_OK; }
void nimble_port_freertos_init(void (*task)(void *)) { (void)task; }
void nimble_port_freertos_deinit(void) { }
void ble_svc_gap_init(void) { }
void ble_svc_gatt_init(void) { }
int ble_svc_gap_device_name_set(const char *name) { return strcmp(name, "ESP32 KVM"); }
int ble_svc_gap_device_appearance_set(uint16_t appearance) { return appearance == 0x03c0 ? 0 : -1; }
void ble_store_config_init(void) { }
int ble_hs_util_ensure_addr(int privacy) { return privacy; }
int ble_hs_id_infer_auto(int privacy, uint8_t *type) { *type = 0; return privacy; }
int ble_store_util_bonded_peers(ble_addr_t *peers, int *count, int maximum)
{ (void)peers; (void)maximum; *count = saved_size ? 1 : 0; return 0; }
int ble_store_util_delete_peer(const ble_addr_t *peer) { (void)peer; return 0; }
int ble_sm_inject_io(uint16_t handle, struct ble_sm_io *io)
{ assert(handle == 17 && io->action == BLE_SM_IOACT_NUMCMP && io->numcmp_accept);
  confirmations++; return 0; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields)
{ assert(fields->num_uuids16 == 1 && fields->uuids16[0].value == 0x1812); return 0; }
int ble_gap_adv_start(uint8_t type, const void *address, uint32_t duration,
                     const struct ble_gap_adv_params *parameters,
                     int (*callback)(struct ble_gap_event *, void *), void *argument)
{ (void)type; (void)address; (void)duration; (void)parameters; (void)argument;
  gap_callback = callback; advertisements++; return 0; }
int ble_gap_terminate(uint16_t handle, uint8_t reason)
{ (void)handle; (void)reason; terminations++; return 0; }
int ble_gap_security_initiate(uint16_t handle) { return handle == 17 ? 0 : -1; }
int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *description)
{ description->conn_handle = handle; description->peer_id_addr = active_peer;
  description->sec_state.encrypted = 1; description->sec_state.bonded = 1;
  description->sec_state.authenticated = 1; return 0; }
int hid_gatt_register(void)
{ hid_channel_init(&channel, NULL, NULL); for (size_t i = 0; i < 2; ++i) hid_channel_init(&other_channels[i], NULL, NULL); return 0; }
hid_channel_t *hid_gatt_channel(void) { return &channel; }
hid_channel_t *hid_gatt_channel_at(uint8_t slot)
{ return slot == 1 ? &channel : slot >= 2 && slot <= HID_GATT_MAX_CONNECTIONS ? &other_channels[slot - 2] : NULL; }
hid_channel_t *hid_gatt_channel_for(uint16_t handle)
{ for (uint8_t slot = 1; slot <= 3; ++slot) { hid_channel_t *c = hid_gatt_channel_at(slot); if (c->connected && c->connection_handle == handle) return c; } return NULL; }
size_t hid_gatt_connection_count(void)
{ size_t count = 0; for (uint8_t slot = 1; slot <= 3; ++slot) count += hid_gatt_channel_at(slot)->connected; return count; }
bool hid_gatt_on_connect(uint16_t handle)
{ if (hid_gatt_channel_for(handle)) return false; for (uint8_t slot = 1; slot <= 3; ++slot) { hid_channel_t *c = hid_gatt_channel_at(slot); if (!c->connected) { hid_channel_connected(c, handle); return true; } } return false; }
void hid_gatt_on_disconnect(uint16_t handle)
{ hid_channel_t *c = hid_gatt_channel_for(handle); if (c) hid_channel_disconnected(c); }
void hid_gatt_on_encryption(uint16_t handle, bool encrypted)
{ hid_channel_t *c = hid_gatt_channel_for(handle); if (c) hid_channel_encrypted(c, encrypted); }
void hid_gatt_on_subscribe(uint16_t handle, uint16_t value_handle, bool enabled)
{ (void)handle; (void)value_handle; (void)enabled; }

int main(void)
{
    active_peer.type = 1; active_peer.val[0] = 42;
    assert(hid_guest_start() == 0);
    assert(ble_hs_cfg.sm_mitm == 1 && ble_hs_cfg.sm_sc == 1);
    assert(ble_hs_cfg.sm_io_cap == BLE_SM_IO_CAP_DISP_YES_NO);
    assert(ble_hs_cfg.store_status_cb(NULL, NULL) != 0);
    ble_hs_cfg.sync_cb();
    assert(advertisements == 1 && !channel.armed);
    struct ble_gap_event connect = {.type = BLE_GAP_EVENT_CONNECT};
    connect.connect.conn_handle = 17;
    gap_callback(&connect, NULL);
    assert(terminations == 1 && !channel.connected);
    hid_guest_pairing_set_events(event_sink, NULL);
    assert(hid_guest_pairing_open() == 0 && last_event.type == HID_GUEST_PAIRING_OPENED);
    time_us = 60000000;
    host_timeout.callback(NULL);
    assert(last_event.type == HID_GUEST_PAIRING_TIMEOUT);
    gap_callback(&connect, NULL);
    assert(terminations == 2 && !channel.connected);
    assert(hid_guest_pairing_open() == 0);
    gap_callback(&connect, NULL);
    assert(channel.connected && !channel.armed && advertisements == 2);
    struct ble_gap_event challenge = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    challenge.passkey.conn_handle = 17;
    challenge.passkey.params.action = BLE_SM_IOACT_NUMCMP;
    challenge.passkey.params.numcmp = 123456;
    gap_callback(&challenge, NULL);
    assert(last_event.type == HID_GUEST_PAIRING_CHALLENGE && last_event.number == 123456);
    assert(last_event.challenge_id != 0);
    assert(hid_guest_pairing_confirm(last_event.challenge_id + 1, true) != 0);
    assert(hid_guest_pairing_confirm(last_event.challenge_id, true) == 0 && confirmations == 1);
    struct ble_gap_event encryption = {.type = BLE_GAP_EVENT_ENC_CHANGE};
    encryption.enc_change.conn_handle = 17;
    gap_callback(&encryption, NULL);
    assert(channel.encrypted && !channel.armed && last_event.type == HID_GUEST_BONDED);
    static const hid_token_t zero_token = {{0}};
    assert(sizeof(last_event.token.bytes) == 16 &&
           memcmp(last_event.token.bytes, zero_token.bytes, HID_PAIRING_TOKEN_LEN) != 0 && saved_size);
    hid_token_t token = last_event.token;
    assert(hid_guest_pairing_open() == 0);
    assert(last_event.type == HID_GUEST_PAIRING_OPENED);
    hid_guest_pairing_cancel();
    assert(!hid_guest_request_ready());
    channel.send = send_report;
    channel.subscribed[HID_REPORT_KEYBOARD] = true;
    channel.subscribed[HID_REPORT_MOUSE] = true;
    channel.subscribed[HID_REPORT_CONSUMER] = true;
    assert(hid_guest_request_ready());
    assert(hid_guest_request_arm());
    const uint8_t keys[HID_KEYBOARD_REPORT_LEN] = {0, 0, 4};
    assert(hid_guest_request_keyboard(keys));
    assert(hid_guest_request_mouse(1, 10, -20, 0, 0));
    assert(hid_guest_request_consumer(0x00e9));
    assert(hid_guest_request_release() && sent_reports == 9);
    uint8_t retained[HID_PAIRING_MAX_BONDS][HID_PAIRING_TOKEN_LEN];
    uint8_t retained_count = 0xff;
    assert(hid_guest_request_retained_bonds(retained, &retained_count));
    assert(retained_count == 1 && memcmp(retained[0], token.bytes, 16) == 0);
    uint8_t current_token[16];
    assert(hid_guest_request_current_bond_token(current_token));
    assert(memcmp(current_token, token.bytes, sizeof(current_token)) == 0);
    active_peer.val[0] = 99;
    memset(current_token, 0xa5, sizeof(current_token));
    assert(!hid_guest_request_current_bond_token(current_token));
    assert(memcmp(current_token, zero_token.bytes, sizeof(current_token)) == 0);
    active_peer.val[0] = 42;
    hid_pairing_t snapshot;
    hid_guest_pairing_snapshot(&snapshot);
    assert(snapshot.bond_count == 1 &&
           memcmp(snapshot.bonds[0].token.bytes, token.bytes, HID_PAIRING_TOKEN_LEN) == 0);
    struct ble_gap_event disconnect = {.type = BLE_GAP_EVENT_DISCONNECT};
    disconnect.disconnect.conn.conn_handle = 17;
    gap_callback(&disconnect, NULL);
    assert(advertisements == 3 && !channel.connected);
    assert(last_event.type == HID_GUEST_DISCONNECTED);
    assert(hid_guest_request_retained_bonds(retained, &retained_count));
    assert(retained_count == 1 && memcmp(retained[0], token.bytes, 16) == 0);
    memset(current_token, 0xa5, sizeof(current_token));
    assert(!hid_guest_request_current_bond_token(current_token));
    assert(memcmp(current_token, zero_token.bytes, sizeof(current_token)) == 0);
    gap_callback(&connect, NULL);
    assert(channel.connected && terminations == 2);
    hold_host = true;
    const uint8_t stale_keys[HID_KEYBOARD_REPORT_LEN] = {0};
    unsigned sent_before_timeout = sent_reports;
    assert(!hid_guest_request_keyboard(stale_keys));
    memset(current_token, 0xa5, sizeof(current_token));
    assert(!hid_guest_request_current_bond_token(current_token));
    assert(memcmp(current_token, zero_token.bytes, sizeof(current_token)) == 0);
    memset(retained, 0xa5, sizeof(retained)); retained_count = 0xff;
    assert(!hid_guest_request_retained_bonds(retained, &retained_count));
    assert(retained_count == 0 && memcmp(retained, (uint8_t[128]){0}, 128) == 0);
    assert(!hid_guest_request_ready());
    assert(sent_reports == sent_before_timeout && host_queue.pending && host_queue.next);
    hold_host = false;
    struct ble_npl_event *expired = host_queue.pending;
    host_queue.pending = host_queue.next; host_queue.next = NULL;
    expired->callback(expired);
    assert(sent_reports == sent_before_timeout);
    host_queue.pending->callback(host_queue.pending);
    host_queue.pending = NULL;
    assert(terminations == 3);
    assert(hid_guest_request_disconnect() == ESP_OK);
    assert(terminations == 3 && host_queue.pending);
    host_queue.pending->callback(host_queue.pending);
    host_queue.pending = NULL;
    assert(terminations == 4 && channel.needs_disconnect);
    assert(hid_guest_pairing_forget(token, false) != 0);
    fail_next_commit = true;
    assert(hid_guest_pairing_forget(token, true) != 0);
    assert(hid_guest_request_retained_bonds(retained, &retained_count));
    assert(retained_count == 1 && memcmp(retained[0], token.bytes, 16) == 0);
    active_peer.val[0] = 99;
    unsigned previous_terminations = terminations;
    assert(hid_guest_pairing_forget(token, true) == 0);
    assert(terminations == previous_terminations && !channel.armed);
    assert(hid_guest_request_retained_bonds(retained, &retained_count));
    assert(retained_count == 0);
    hid_channel_disconnected(&channel);
    assert(hid_gatt_on_connect(17) && hid_gatt_on_connect(18));
    hid_channel_t *second = hid_gatt_channel_at(2);
    second->send = send_report;
    hid_gatt_on_encryption(18, true);
    second->subscribed[HID_REPORT_KEYBOARD] = true;
    second->subscribed[HID_REPORT_MOUSE] = true;
    second->subscribed[HID_REPORT_CONSUMER] = true;
    assert(hid_guest_request_ready_slot(2));
    assert(hid_guest_request_arm_slot(2));
    assert(hid_guest_request_keyboard_slot(2, keys) && last_report_handle == 18);
    assert(!channel.armed && !channel.keyboard[2]);
    assert(hid_guest_request_release_slot(2) && !second->armed);
    hold_host = true;
    unsigned prior_terminations = terminations;
    assert(!hid_guest_request_keyboard_slot(2, keys));
    assert(!channel.needs_disconnect && host_queue.pending && host_queue.next);
    hold_host = false;
    struct ble_npl_event *late = host_queue.pending;
    host_queue.pending = host_queue.next; host_queue.next = NULL;
    late->callback(late);
    host_queue.pending->callback(host_queue.pending);
    host_queue.pending = NULL;
    assert(second->needs_disconnect && !channel.needs_disconnect &&
           terminations == prior_terminations + 1);
    return 0;
}
