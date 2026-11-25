/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Checks the pairing-scan helpers: advertising-data parsing (UUID lists,
 * appearance, names, malformed structures, exact-size buffers) and the bounded,
 * address-deduplicated candidate list with stable row indexes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "central_scan.h"

static int failures;
#define CHECK(c)                                               \
    do {                                                       \
        if (!(c)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            failures++;                                        \
        }                                                      \
    } while (0)

static void parse_exact(const uint8_t *src, size_t len, central_adv_info_t *out)
{
    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
    if (!copy) exit(2);
    if (len) memcpy(copy, src, len);
    central_adv_parse(copy, len, out);
    free(copy);
}

static void test_adv_parse(void)
{
    /* Flags, incomplete UUID16 list {0x180F, 0x1812}, appearance 0x03C1, complete name. */
    static const uint8_t adv[] = {0x02, 0x01, 0x06, 0x05, 0x02, 0x0F, 0x18, 0x12, 0x18, 0x03, 0x19, 0xC1,
                                  0x03, 0x05, 0x09, 'K', 'e', 'y', 's'};
    central_adv_info_t info;
    parse_exact(adv, sizeof(adv), &info);
    CHECK(info.hid_service && info.has_appearance && info.appearance == 0x03C1);
    CHECK(strcmp(info.name, "Keys") == 0);

    /* Short name (0x08), complete UUID list without HID. */
    static const uint8_t other[] = {0x03, 0x03, 0x0F, 0x18, 0x04, 0x08, 'M', 'X', 0x01};
    parse_exact(other, sizeof(other), &info);
    CHECK(!info.hid_service && !info.has_appearance && strcmp(info.name, "MX?") == 0);

    /* A structure whose length runs past the end stops parsing; earlier fields stay. */
    static const uint8_t truncated[] = {0x03, 0x19, 0xC2, 0x03, 0x09, 0x09, 'A', 'B'};
    parse_exact(truncated, sizeof(truncated), &info);
    CHECK(info.has_appearance && info.appearance == 0x03C2 && info.name[0] == '\0');

    /* Zero-length structure ends the data; an odd UUID16 list length is ignored. */
    static const uint8_t odd[] = {0x04, 0x03, 0x12, 0x18, 0x00, 0x00, 0x02, 0x19, 0xC1};
    parse_exact(odd, sizeof(odd), &info);
    CHECK(!info.hid_service && !info.has_appearance);

    /* Names longer than 23 bytes are truncated and terminated. */
    uint8_t longname[2 + 30];
    longname[0] = 31;
    longname[1] = 0x09;
    memset(longname + 2, 'n', 30);
    parse_exact(longname, sizeof(longname), &info);
    CHECK(strlen(info.name) == CENTRAL_SCAN_NAME_BYTES - 1);

    /* Every truncation of a valid payload stays in bounds. */
    for (size_t len = 0; len <= sizeof(adv); ++len) parse_exact(adv, len, &info);
    parse_exact(NULL, 0, &info);
    CHECK(!info.hid_service && info.name[0] == '\0');
}

static central_adv_info_t info_of(bool hid, uint16_t appearance, const char *name)
{
    central_adv_info_t info;
    memset(&info, 0, sizeof(info));
    info.hid_service = hid;
    info.has_appearance = appearance != 0;
    info.appearance = appearance;
    if (name) snprintf(info.name, sizeof(info.name), "%s", name);
    return info;
}

static void test_scan_list(void)
{
    central_scan_list_t list;
    central_scan_clear(&list);
    uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    central_adv_info_t plain = info_of(false, 0, "Speaker");
    CHECK(central_scan_offer(&list, a, 0, -40, &plain) == -1 && list.count == 0);
    /* Generic HID appearance alone is not a keyboard or mouse. */
    central_adv_info_t generic = info_of(false, 0x03C0, NULL);
    CHECK(central_scan_offer(&list, a, 0, -40, &generic) == -1);

    central_adv_info_t hid = info_of(true, 0, NULL);
    CHECK(central_scan_offer(&list, a, 0, -60, &hid) == 0 && list.count == 1);
    CHECK(list.entries[0].kind == 0 && list.entries[0].rssi == -60 && list.entries[0].name[0] == '\0');
    /* Same address: RSSI only -> not reported again. */
    CHECK(central_scan_offer(&list, a, 0, -50, &hid) == -1 && list.entries[0].rssi == -50);
    /* The scan response brings name and appearance -> reported with the same index. */
    central_adv_info_t rsp = info_of(false, 0x03C1, "Desk KB");
    CHECK(central_scan_offer(&list, a, 0, -52, &rsp) == 0 && list.count == 1);
    CHECK(list.entries[0].kind == DOUBTECH_HID_CENTRAL_KEYBOARD && strcmp(list.entries[0].name, "Desk KB") == 0);
    CHECK(central_scan_offer(&list, a, 0, -52, &rsp) == -1);

    /* Same bytes, other address type: a different device. */
    central_adv_info_t mouse = info_of(false, 0x03C2, "Mouse");
    CHECK(central_scan_offer(&list, a, 1, -70, &mouse) == 1);
    CHECK(list.entries[1].kind == DOUBTECH_HID_CENTRAL_MOUSE && list.entries[1].addr_type == 1);

    /* Bounded at eight rows; earlier indexes never move. */
    for (uint8_t i = 2; i < CENTRAL_SCAN_MAX; ++i) {
        uint8_t b[6] = {9, 9, 9, 9, 9, i};
        CHECK(central_scan_offer(&list, b, 0, -80, &hid) == i);
    }
    uint8_t extra[6] = {7, 7, 7, 7, 7, 7};
    CHECK(central_scan_offer(&list, extra, 0, -10, &hid) == -1 && list.count == CENTRAL_SCAN_MAX);
    CHECK(memcmp(list.entries[0].addr, a, 6) == 0 && list.entries[0].addr_type == 0);
    /* An unknown address with only a name (scan response) is not listed. */
    central_scan_clear(&list);
    central_adv_info_t name_only = info_of(false, 0, "Thing");
    CHECK(central_scan_offer(&list, extra, 0, -10, &name_only) == -1 && list.count == 0);
    /* The HID gamepad appearance (spec filter) is listed with an unknown kind. */
    central_adv_info_t gamepad = info_of(false, 0x03C4, "Pad");
    CHECK(central_scan_offer(&list, extra, 0, -10, &gamepad) == 0 && list.entries[0].kind == 0);
}

int main(void)
{
    test_adv_parse();
    test_scan_list();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
