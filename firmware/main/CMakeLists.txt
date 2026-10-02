# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Registers the safe, non-routing firmware entry point for initial build verification.
idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")
