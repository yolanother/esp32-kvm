/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Verifies configurable advertised identity, Device Information strings and
 * bounded validation against the NimBLE mocks. Each case runs in its own
 * process because the service can start only once; pass the case name. */
#define main legacy_doubtech_hid_test
#include "test_hid_guest.c"
#undef main

static void no_side_effects(void)
{
    assert(nvs_opens == 0 && adv_field_sets == 0 && gatt_registrations == 0);
    assert(gap_name[0] == '\0' && dis_manufacturer[0] == '\0');
}

static void reject_name(const char *name)
{
    doubtech_hid_config_t config = {name, NULL, NULL, NULL};
    check_gap_name = false;
    assert(doubtech_hid_start(&config) == ESP_ERR_INVALID_ARG);
    no_side_effects();
}

static void config_null_uses_defaults(void)
{
    check_gap_name = false;
    assert(doubtech_hid_start(NULL) == 0);
    ble_hs_cfg.sync_cb();
    assert(adv_field_sets == 1 && strcmp(adv_name, "BLE HID") == 0 && adv_name_len == 7);
    assert(strcmp(gap_name, "BLE HID") == 0);
    assert(strcmp(dis_manufacturer, "DoubTech") == 0 && strcmp(dis_model, "BLE HID") == 0);
}

static void name_exactly_20_bytes_accepted(void)
{
    char name[] = "12345678901234567890";
    doubtech_hid_config_t config = {name, NULL, NULL, NULL};
    check_gap_name = false;
    assert(doubtech_hid_start(&config) == 0);
    memset(name, 'x', sizeof(name) - 1); /* The library keeps its own copy. */
    ble_hs_cfg.sync_cb();
    assert(adv_name_len == 20 && memcmp(adv_name, "12345678901234567890", 20) == 0);
    assert(strcmp(gap_name, "12345678901234567890") == 0);
}

static void name_21_bytes_rejected(void) { reject_name("123456789012345678901"); }
static void name_empty_rejected(void) { reject_name(""); }

static void name_multibyte_counts_bytes(void)
{
    /* Twelve two-byte characters are 24 bytes. */
    reject_name("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
}

static void name_multibyte_20_bytes_accepted(void)
{
    const char *name = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    doubtech_hid_config_t config = {name, NULL, NULL, NULL};
    check_gap_name = false;
    assert(doubtech_hid_start(&config) == 0);
    ble_hs_cfg.sync_cb();
    assert(adv_name_len == 20 && memcmp(adv_name, name, 20) == 0);
}

static void dis_strings_bounded(void)
{
    const char *long_text = "123456789012345678901234567890123"; /* 33 bytes */
    doubtech_hid_config_t long_manufacturer = {NULL, long_text, NULL, NULL};
    doubtech_hid_config_t long_model = {NULL, NULL, long_text, NULL};
    doubtech_hid_config_t empty_manufacturer = {NULL, "", NULL, NULL};
    doubtech_hid_config_t empty_model = {NULL, NULL, "", NULL};
    doubtech_hid_config_t bad_namespace = {NULL, NULL, NULL, "0123456789abcdef"};
    doubtech_hid_config_t empty_namespace = {NULL, NULL, NULL, ""};
    check_gap_name = false;
    assert(doubtech_hid_start(&long_manufacturer) == ESP_ERR_INVALID_ARG);
    assert(doubtech_hid_start(&long_model) == ESP_ERR_INVALID_ARG);
    assert(doubtech_hid_start(&empty_manufacturer) == ESP_ERR_INVALID_ARG);
    assert(doubtech_hid_start(&empty_model) == ESP_ERR_INVALID_ARG);
    assert(doubtech_hid_start(&bad_namespace) == ESP_ERR_INVALID_ARG);
    assert(doubtech_hid_start(&empty_namespace) == ESP_ERR_INVALID_ARG);
    no_side_effects();
    doubtech_hid_config_t exact = {NULL, "12345678901234567890123456789012",
                                   "12345678901234567890123456789012", NULL};
    assert(doubtech_hid_start(&exact) == 0);
    assert(strlen(dis_manufacturer) == 32 && strlen(dis_model) == 32);
}

static void advertised_name_and_dis_follow_config(void)
{
    char name[] = "Desk Keys";
    char manufacturer[] = "Acme";
    char model[] = "Model 7";
    doubtech_hid_config_t config = {name, manufacturer, model, NULL};
    check_gap_name = false;
    assert(doubtech_hid_start(&config) == 0);
    memset(name, 0, sizeof(name));
    memset(manufacturer, 0, sizeof(manufacturer));
    memset(model, 0, sizeof(model));
    ble_hs_cfg.sync_cb();
    assert(adv_name_len == 9 && memcmp(adv_name, "Desk Keys", 9) == 0);
    assert(strcmp(gap_name, "Desk Keys") == 0);
    assert(strcmp(dis_manufacturer, "Acme") == 0 && strcmp(dis_model, "Model 7") == 0);
}

static void legacy_identity_is_unchanged(void)
{
    assert(doubtech_hid_start(&legacy_config) == 0);
    ble_hs_cfg.sync_cb();
    assert(adv_name_len == 9 && memcmp(adv_name, "ESP32 KVM", 9) == 0);
    assert(strcmp(gap_name, "ESP32 KVM") == 0);
    assert(strcmp(dis_manufacturer, "ESP32 KVM") == 0 && strcmp(dis_model, "ESP32 KVM S3") == 0);
}

static void start_twice_is_safe(void)
{
    active_peer.type = 1; active_peer.val[0] = 42;
    assert(doubtech_hid_start(&legacy_config) == 0);
    ble_hs_cfg.sync_cb();
    struct ble_gap_event connect = {.type = BLE_GAP_EVENT_CONNECT};
    connect.connect.conn_handle = 17;
    gap_callback(&connect, NULL);
    assert(channel.connected);
    unsigned registrations = gatt_registrations, opens = nvs_opens;
    /* The current implementation refuses a second start outright. */
    assert(doubtech_hid_start(&legacy_config) == ESP_ERR_INVALID_STATE);
    doubtech_hid_config_t other = {"Other", "Other", "Other", NULL};
    assert(doubtech_hid_start(&other) == ESP_ERR_INVALID_STATE);
    assert(gatt_registrations == registrations && gatt_registrations == 1);
    assert(nvs_opens == opens && channel.connected);
    assert(strcmp(gap_name, "ESP32 KVM") == 0);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"config_null_uses_defaults", config_null_uses_defaults},
        {"name_exactly_20_bytes_accepted", name_exactly_20_bytes_accepted},
        {"name_21_bytes_rejected", name_21_bytes_rejected},
        {"name_empty_rejected", name_empty_rejected},
        {"name_multibyte_counts_bytes", name_multibyte_counts_bytes},
        {"name_multibyte_20_bytes_accepted", name_multibyte_20_bytes_accepted},
        {"dis_strings_bounded", dis_strings_bounded},
        {"advertised_name_and_dis_follow_config", advertised_name_and_dis_follow_config},
        {"legacy_identity_is_unchanged", legacy_identity_is_unchanged},
        {"start_twice_is_safe", start_twice_is_safe},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
        if (argc == 2 && strcmp(argv[1], cases[index].name) == 0) {
            cases[index].run();
            return 0;
        }
    return 2;
}
