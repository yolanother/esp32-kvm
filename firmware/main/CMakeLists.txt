# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Registers the disarmed USB and BLE service startup entry point.
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES transport hid)
