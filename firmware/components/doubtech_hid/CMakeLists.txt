# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the composite HID report gate and NimBLE GATT service for one guest.
idf_component_register(SRCS "core/hid_report.c" "core/hid_pairing.c" "ble/hid_gatt.c" "ble/hid_guest.c" "ble/hid_guest_rpc.c" "ble/hid_pairing_store.c" INCLUDE_DIRS "core" "ble" REQUIRES bt nvs_flash esp_timer)
