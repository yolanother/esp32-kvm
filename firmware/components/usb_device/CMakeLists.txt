# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the USB device support; for now only the pure boot-mode decision.
idf_component_register(SRCS "usb_mode.c" INCLUDE_DIRS "." REQUIRES log)
