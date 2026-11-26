/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * The byte link under the USB worker: whether a host is attached, a bounded
 * read and a bounded write. USB Serial/JTAG is the only link today; a later
 * TinyUSB CDC link plugs in behind the same calls. The header is portable C
 * so the write loop is host-tested without ESP-IDF. */
#ifndef ESP32_KVM_TRANSPORT_LINK_H
#define ESP32_KVM_TRANSPORT_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** One byte link. Waits are FreeRTOS ticks. read and write return the byte
 * count moved (0 when none moved within the wait) or a negative value on an
 * error; neither buffers beyond what the underlying driver already does. */
typedef struct {
    bool (*connected)(void *context);
    int (*read)(void *context, uint8_t *buf, size_t cap, uint32_t wait_ticks);
    int (*write)(void *context, const uint8_t *buf, size_t len, uint32_t wait_ticks);
    void *context;
} kvm_transport_link_t;

/** Writes all `length` bytes, offering the remainder after each partial write.
 * Each try waits at most `per_try_ticks`. A try that moves no byte counts
 * against `max_tries` (tries that move bytes do not), so time spent without
 * progress is at most per_try_ticks * max_tries. A negative return, or one
 * larger than the bytes offered, gives up at once. Returns true when every
 * byte was accepted (at once for length 0, without calling the link); false
 * on a NULL link or write callback, NULL bytes with a non-zero length,
 * max_tries 0, an error, or a spent budget. */
bool kvm_transport_link_write_all(const kvm_transport_link_t *link, const uint8_t *bytes,
                                  size_t length, uint32_t per_try_ticks, uint32_t max_tries);

/** USB Serial/JTAG: a thin wrapper over usb_serial_jtag_is_connected,
 * usb_serial_jtag_read_bytes and usb_serial_jtag_write_bytes. The driver must
 * be installed before the link is used. ESP-IDF builds only. */
const kvm_transport_link_t *kvm_transport_link_usj(void);

#endif /* ESP32_KVM_TRANSPORT_LINK_H */
