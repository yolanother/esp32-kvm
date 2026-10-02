# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the bounded M0 protocol loopback on ESP32-S3 fixed USB Serial/JTAG CDC.
idf_component_register(SRCS "transport_core.c" "transport_usb_serial_jtag.c"
    INCLUDE_DIRS "." "../../../protocol/schema"
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_hw_support)
