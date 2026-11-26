/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * The USB Serial/JTAG byte link: each call forwards to the IDF driver with
 * the same arguments and return value, no extra buffering. */
#include "transport_link.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

static bool usj_connected(void *context)
{
    (void)context;
    return usb_serial_jtag_is_connected();
}

static int usj_read(void *context, uint8_t *buf, size_t cap, uint32_t wait_ticks)
{
    (void)context;
    return usb_serial_jtag_read_bytes(buf, (uint32_t)cap, (TickType_t)wait_ticks);
}

static int usj_write(void *context, const uint8_t *buf, size_t len, uint32_t wait_ticks)
{
    (void)context;
    return usb_serial_jtag_write_bytes(buf, len, (TickType_t)wait_ticks);
}

static const kvm_transport_link_t usj_link = {
    .connected = usj_connected, .read = usj_read, .write = usj_write, .context = NULL,
};

const kvm_transport_link_t *kvm_transport_link_usj(void)
{
    return &usj_link;
}
