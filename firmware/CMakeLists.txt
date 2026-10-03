# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Defines the ESP-IDF application and discovers local board, transport, HID, router, and display components.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0-m1")
project(esp32_kvm)
