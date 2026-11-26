# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Builds the USB device support: the pure boot-mode decision, the composite descriptors and the
# state-based HID sink (router supplies kvm_router_input_t; router itself depends on nothing).
idf_component_register(SRCS "usb_mode.c" "usb_descriptors.c" "usb_hid_sink.c" INCLUDE_DIRS "." REQUIRES log router)
