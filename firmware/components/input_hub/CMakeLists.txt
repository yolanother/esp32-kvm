# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the portable input arbitration hub; board wiring supplies output callbacks.
idf_component_register(SRCS "input_hub.c" INCLUDE_DIRS "." REQUIRES router)
