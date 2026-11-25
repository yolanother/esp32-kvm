# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the HID-over-GATT central support: pure boot and report-map parsers.
idf_component_register(SRCS "hogp_parse.c" INCLUDE_DIRS ".")
