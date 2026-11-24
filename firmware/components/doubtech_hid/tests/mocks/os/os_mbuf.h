/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Small mbuf shape and operations used to compile and exercise HID GATT access
 * callbacks without an ESP-IDF toolchain or a radio. */
#ifndef TEST_OS_MBUF_H
#define TEST_OS_MBUF_H
#include <stddef.h>
#include <stdint.h>
struct os_mbuf { uint8_t data[512]; size_t len; };
#define OS_MBUF_PKTLEN(value) ((value)->len)
int os_mbuf_append(struct os_mbuf *buffer, const void *data, size_t length);
int os_mbuf_copydata(const struct os_mbuf *buffer, size_t offset, size_t length, void *out);
#endif
