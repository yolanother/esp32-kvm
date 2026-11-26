# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the bounded M0 protocol loopback on ESP32-S3 fixed USB Serial/JTAG CDC.
idf_component_register(SRCS "transport_core.c" "transport_display_status.c" "transport_usb_serial_jtag.c" "transport_diagnostics.c" "router_hid_bridge.c" "transport_hub.c"
    INCLUDE_DIRS "." "../../../protocol/schema"
    REQUIRES router input_hub display usb_device
    PRIV_REQUIRES esp_app_format esp_driver_usb_serial_jtag esp_hw_support doubtech_hid doubtech_hid_central esp_timer)
