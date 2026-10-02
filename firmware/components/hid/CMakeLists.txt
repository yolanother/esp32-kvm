# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the composite HID report gate and NimBLE GATT service for one guest.
idf_component_register(SRCS "hid_report.c" "hid_gatt.c" "hid_guest.c" INCLUDE_DIRS "." REQUIRES bt nvs_flash)
