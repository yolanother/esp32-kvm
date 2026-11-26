# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the USB device support; for now the pure boot-mode decision and the composite descriptors.
idf_component_register(SRCS "usb_mode.c" "usb_descriptors.c" INCLUDE_DIRS "." REQUIRES log)
