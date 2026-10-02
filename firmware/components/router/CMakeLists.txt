# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the portable serialized routing actor; board wiring supplies output callbacks.
idf_component_register(SRCS "router.c" INCLUDE_DIRS ".")
