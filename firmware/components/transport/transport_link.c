/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * The link-independent write loop shared by every byte link (portable C). */
#include "transport_link.h"

bool kvm_transport_link_write_all(const kvm_transport_link_t *link, const uint8_t *bytes,
                                  size_t length, uint32_t per_try_ticks, uint32_t max_tries)
{
    if (!link || !link->write) return false;
    if (length == 0) return true;
    if (!bytes) return false;
    size_t at = 0;
    uint32_t failed = 0;
    while (at < length) {
        if (failed >= max_tries) return false;
        int written = link->write(link->context, bytes + at, length - at, per_try_ticks);
        if (written < 0 || (size_t)written > length - at) return false;
        if (written == 0) {
            failed++;
            continue;
        }
        at += (size_t)written;
    }
    return true;
}
