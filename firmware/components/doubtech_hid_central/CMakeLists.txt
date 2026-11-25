# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the HID-over-GATT central: pure parsers, registry, discovery machine and
# scan list, plus the NimBLE adapter that shares the host doubtech_hid starts.
idf_component_register(
    SRCS "hogp_parse.c" "central_registry.c" "central_store.c" "central_gatt.c" "central_scan.c" "central_ble.c"
    INCLUDE_DIRS "include" "."
    REQUIRES doubtech_hid bt nvs_flash)
# Keep the adapter in the link even before the application calls it, so the
# firmware build resolves every NimBLE symbol it uses.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u doubtech_hid_central_start")
