#ifndef TEST_BLE_SVC_HID_H
#define TEST_BLE_SVC_HID_H
#include <stdint.h>
#define BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE 0x2a4e
#define BLE_SVC_HID_CHR_UUID16_HID_CTRL_PT 0x2a4c
#define BLE_SVC_HID_CHR_UUID16_RPT 0x2a4d
#define BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP 0x2a22
#define BLE_SVC_HID_PROTO_MODE_REPORT 1
#define BLE_SVC_HID_RPT_TYPE_INPUT 1
#define BLE_SVC_HID_RPT_TYPE_OUTPUT 2
struct report { uint8_t data[256], len, type, id; uint16_t handle; };
struct ble_svc_hid_params {
    uint32_t hid_info;
    uint8_t report_map[512], report_map_len;
    uint16_t external_rpt_ref;
    uint8_t proto_mode_present, proto_mode, kbd_inp_present, kbd_out_present;
    struct report rpts[3];
    uint8_t rpts_len;
};
int ble_svc_hid_add(struct ble_svc_hid_params params);
void ble_svc_hid_init(void);
#endif
