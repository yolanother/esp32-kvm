/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks the input-device registry and its NVS store against an in-memory NVS
 * model: round trip, corruption, truncation, version, failed saves, and
 * isolation from the legacy guest bond namespace. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "central_registry.h"
#include "central_store.h"
#include "nvs.h"

static int failures;
#define CHECK(c)                                               \
    do {                                                       \
        if (!(c)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            failures++;                                        \
        }                                                      \
    } while (0)

/* ---- in-memory NVS model ---- */
#define NS_MAX 4
typedef struct {
    char ns[32];
    unsigned char data[256];
    size_t len;
    int has_blob;
    int exists;
} model_ns_t;
static model_ns_t model[NS_MAX];
static unsigned char pending[256];
static size_t pending_len;
static int handle_ns[8];
static int fail_set, fail_commit, touched_guest, guest_opened_rw;

static void model_reset(void)
{
    memset(model, 0, sizeof(model));
    pending_len = 0;
    fail_set = fail_commit = touched_guest = guest_opened_rw = 0;
}

static int model_find(const char *name, int create)
{
    for (int i = 0; i < NS_MAX; ++i)
        if (model[i].exists && strcmp(model[i].ns, name) == 0) return i;
    if (!create) return -1;
    for (int i = 0; i < NS_MAX; ++i)
        if (!model[i].exists) {
            model[i].exists = 1;
            memcpy(model[i].ns, name, strlen(name) + 1);
            return i;
        }
    return -1;
}

int nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    if (strcmp(name, "kvm_bonds") == 0) {
        touched_guest = 1;
        if (mode == NVS_READWRITE) guest_opened_rw = 1;
    }
    int idx = model_find(name, mode == NVS_READWRITE);
    if (idx < 0) return ESP_ERR_NVS_NOT_FOUND;
    *handle = (nvs_handle_t)(idx + 1);
    handle_ns[idx] = idx;
    return 0;
}
int nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    model_ns_t *m = &model[handle - 1];
    (void)key;
    if (!m->has_blob) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < m->len) return 0x110c;
    memcpy(value, m->data, m->len);
    *length = m->len;
    return 0;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle; (void)key;
    if (fail_set) return ESP_FAIL;
    memcpy(pending, value, length);
    pending_len = length;
    return 0;
}
int nvs_commit(nvs_handle_t handle)
{
    if (fail_commit) return ESP_FAIL;
    model_ns_t *m = &model[handle - 1];
    memcpy(m->data, pending, pending_len);
    m->len = pending_len;
    m->has_blob = 1;
    return 0;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

/* ---- helpers ---- */
static dthid_input_record_t make(dthid_input_kind_t kind, uint8_t last, const char *name)
{
    dthid_input_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.addr[0] = 0xC0; rec.addr[5] = last;
    rec.addr_type = 1;
    rec.kind = kind;
    memcpy(rec.name, name, strlen(name) + 1);
    return rec;
}

static model_ns_t *inputs_ns(void) { return &model[model_find("dthid_inputs", 0)]; }

static void test_registry(void)
{
    central_registry_t r;
    memset(&r, 0, sizeof(r));
    dthid_input_record_t out, kb = make(DTHID_INPUT_KEYBOARD, 1, "Kbd"), mouse = make(DTHID_INPUT_MOUSE, 2, "Mouse");
    CHECK(!central_registry_get(&r, DTHID_INPUT_KEYBOARD, &out));
    CHECK(central_registry_set(&r, &kb));
    CHECK(central_registry_set(&r, &mouse));
    CHECK(central_registry_get(&r, DTHID_INPUT_KEYBOARD, &out) && out.addr[5] == 1 && strcmp(out.name, "Kbd") == 0);
    CHECK(central_registry_get(&r, DTHID_INPUT_MOUSE, &out) && out.addr[5] == 2);
    /* replace keeps one record per kind and leaves the other kind alone */
    dthid_input_record_t kb2 = make(DTHID_INPUT_KEYBOARD, 9, "Kbd2");
    CHECK(central_registry_set(&r, &kb2));
    CHECK(central_registry_get(&r, DTHID_INPUT_KEYBOARD, &out) && out.addr[5] == 9);
    CHECK(central_registry_get(&r, DTHID_INPUT_MOUSE, &out) && out.addr[5] == 2);
    /* combo device: same address under both kinds is two independent records */
    dthid_input_record_t combo_k = make(DTHID_INPUT_KEYBOARD, 7, "Combo"), combo_m = make(DTHID_INPUT_MOUSE, 7, "Combo");
    CHECK(central_registry_set(&r, &combo_k) && central_registry_set(&r, &combo_m));
    CHECK(central_registry_forget(&r, DTHID_INPUT_KEYBOARD));
    CHECK(!central_registry_get(&r, DTHID_INPUT_KEYBOARD, &out));
    CHECK(central_registry_get(&r, DTHID_INPUT_MOUSE, &out) && out.addr[5] == 7);
    CHECK(!central_registry_forget(&r, DTHID_INPUT_KEYBOARD));
    /* invalid input */
    dthid_input_record_t bad = make((dthid_input_kind_t)3, 1, "x");
    CHECK(!central_registry_set(&r, &bad));
    CHECK(!central_registry_set(NULL, &kb) && !central_registry_set(&r, NULL));
    CHECK(!central_registry_get(&r, (dthid_input_kind_t)0, &out));
    /* unterminated name is bounded and terminated */
    dthid_input_record_t longname;
    memset(&longname, 'A', sizeof(longname));
    longname.kind = DTHID_INPUT_MOUSE;
    CHECK(central_registry_set(&r, &longname));
    CHECK(central_registry_get(&r, DTHID_INPUT_MOUSE, &out));
    CHECK(out.name[DTHID_INPUT_NAME_BYTES - 1] == '\0' && strlen(out.name) == DTHID_INPUT_NAME_BYTES - 1);
}

static central_registry_t two_records(void)
{
    central_registry_t r;
    memset(&r, 0, sizeof(r));
    dthid_input_record_t kb = make(DTHID_INPUT_KEYBOARD, 1, "Kbd"), mouse = make(DTHID_INPUT_MOUSE, 2, "Mouse");
    central_registry_set(&r, &kb);
    central_registry_set(&r, &mouse);
    return r;
}

static void test_roundtrip(void)
{
    model_reset();
    central_registry_t saved = two_records(), loaded;
    CHECK(central_store_save(NULL, &saved) == ESP_OK);
    CHECK(central_store_load(NULL, &loaded) == ESP_OK);
    dthid_input_record_t a, b;
    CHECK(central_registry_get(&loaded, DTHID_INPUT_KEYBOARD, &a) && strcmp(a.name, "Kbd") == 0 && a.addr_type == 1);
    CHECK(central_registry_get(&loaded, DTHID_INPUT_MOUSE, &b) && b.addr[5] == 2 && b.addr[0] == 0xC0);
    /* only one kind saved */
    central_registry_forget(&saved, DTHID_INPUT_KEYBOARD);
    CHECK(central_store_save(NULL, &saved) == ESP_OK);
    CHECK(central_store_load(NULL, &loaded) == ESP_OK);
    CHECK(!central_registry_get(&loaded, DTHID_INPUT_KEYBOARD, &a) && central_registry_get(&loaded, DTHID_INPUT_MOUSE, &b));
    /* empty registry */
    memset(&saved, 0, sizeof(saved));
    CHECK(central_store_save(NULL, &saved) == ESP_OK && central_store_load(NULL, &loaded) == ESP_OK);
    CHECK(!loaded.used[0] && !loaded.used[1]);
    CHECK(!touched_guest);
}

static void expect_empty_load(void)
{
    central_registry_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded));
    CHECK(central_store_load(NULL, &loaded) == ESP_OK);
    CHECK(!loaded.used[0] && !loaded.used[1]);
}

static void test_bad_blobs(void)
{
    central_registry_t saved = two_records();
    model_reset();
    expect_empty_load(); /* nothing stored */
    CHECK(central_store_save(NULL, &saved) == ESP_OK);
    model_ns_t *m = inputs_ns();
    size_t good_len = m->len;
    unsigned char good[256];
    memcpy(good, m->data, good_len);
    /* flipped payload byte: CRC mismatch */
    m->data[20] ^= 0x01;
    expect_empty_load();
    memcpy(m->data, good, good_len);
    /* flipped header crc */
    m->data[12] ^= 0xFF;
    expect_empty_load();
    memcpy(m->data, good, good_len);
    /* every truncation */
    for (size_t len = 0; len < good_len; ++len) {
        m->len = len;
        expect_empty_load();
    }
    /* trailing bytes */
    m->len = good_len;
    memcpy(m->data, good, good_len);
    m->data[good_len] = 0;
    m->len = good_len + 1;
    expect_empty_load();
    /* oversized blob (NVS reports invalid length) */
    m->len = 200;
    expect_empty_load();
    /* unknown version with a valid layout and a correct-looking blob */
    m->len = good_len;
    memcpy(m->data, good, good_len);
    m->data[4] = 2;
    expect_empty_load();
    /* wrong magic */
    memcpy(m->data, good, good_len);
    m->data[0] ^= 0xFF;
    expect_empty_load();
    /* count above the limit */
    memcpy(m->data, good, good_len);
    m->data[8] = 3;
    expect_empty_load();
    /* intact blob still loads */
    memcpy(m->data, good, good_len);
    central_registry_t loaded;
    CHECK(central_store_load(NULL, &loaded) == ESP_OK && loaded.used[0] && loaded.used[1]);
}

static void test_failed_save_keeps_old_blob(void)
{
    model_reset();
    central_registry_t first = two_records(), second = two_records(), loaded;
    dthid_input_record_t changed = make(DTHID_INPUT_KEYBOARD, 0x55, "New");
    central_registry_set(&second, &changed);
    CHECK(central_store_save(NULL, &first) == ESP_OK);
    fail_set = 1;
    CHECK(central_store_save(NULL, &second) != ESP_OK);
    fail_set = 0;
    fail_commit = 1;
    CHECK(central_store_save(NULL, &second) != ESP_OK);
    fail_commit = 0;
    dthid_input_record_t out;
    CHECK(central_store_load(NULL, &loaded) == ESP_OK);
    CHECK(central_registry_get(&loaded, DTHID_INPUT_KEYBOARD, &out) && out.addr[5] == 1);
}

static void test_namespaces(void)
{
    model_reset();
    central_registry_t saved = two_records(), loaded;
    CHECK(central_store_save("123456789012345", &saved) == ESP_OK); /* 15 chars ok */
    CHECK(central_store_load("123456789012345", &loaded) == ESP_OK && loaded.used[0]);
    CHECK(central_store_save("1234567890123456", &saved) == ESP_ERR_INVALID_ARG);
    CHECK(central_store_load("1234567890123456", &loaded) == ESP_ERR_INVALID_ARG);
    CHECK(central_store_save("", &saved) == ESP_ERR_INVALID_ARG);
    CHECK(central_store_save(NULL, NULL) == ESP_ERR_INVALID_ARG);
    /* saving never touches the guest bond namespace, even when asked to */
    CHECK(central_store_save("kvm_bonds", &saved) == ESP_ERR_INVALID_ARG);
    CHECK(central_store_load("kvm_bonds", &loaded) == ESP_ERR_INVALID_ARG);
    CHECK(central_store_save(NULL, &saved) == ESP_OK);
    CHECK(model_find("dthid_inputs", 0) >= 0);
    CHECK(!touched_guest && !guest_opened_rw);
}

int main(void)
{
    test_registry();
    test_roundtrip();
    test_bad_blobs();
    test_failed_save_keeps_old_blob();
    test_namespaces();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_central_registry: all passed\n");
    return 0;
}
