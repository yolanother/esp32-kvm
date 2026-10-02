/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Defines the disarmed firmware USB loopback protocol core. It accepts bounded
 * COBS frames, validates CRC32C and sessions, and dispatches authenticated
 * routing commands to a bound serialized router. A USB CDC adapter owns I/O. */
#ifndef ESP32_KVM_TRANSPORT_CORE_H
#define ESP32_KVM_TRANSPORT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol_v1.h"
#include "router.h"

/** Maximum encoded frame body plus delimiter for the version-one envelope. */
#define KVM_TRANSPORT_ENCODED_CAPACITY (KVM_PROTOCOL_MAX_FRAME + KVM_PROTOCOL_MAX_FRAME / 254u + 2u)

/** Called synchronously when a complete binary response is ready to transmit. */
typedef void (*kvm_transport_send_fn)(void *context, const uint8_t *bytes, size_t length);
/** Supplies monotonic firmware milliseconds for lease and input age checks. */
typedef uint64_t (*kvm_transport_now_fn)(void *context);

/** Private stream and handshake state for one USB connection. */
typedef struct {
    uint8_t rx[KVM_TRANSPORT_ENCODED_CAPACITY];
    size_t rx_length;
    bool draining;
    bool session_open;
    uint64_t session_id;
    char board_id[33];
    char firmware_version[33];
    kvm_transport_send_fn send;
    void *send_context;
    kvm_router_t *router;
    kvm_transport_now_fn now;
    void *now_context;
    uint64_t last_status_ms;
    uint32_t status_generation;
    uint8_t status_slot;
    bool status_armed;
    bool status_fault;
    bool status_ready;
} kvm_transport_core_t;

/** Initializes a disarmed loopback core with a nonzero, caller-generated session. */
bool kvm_transport_core_init(kvm_transport_core_t *core, const char *board_id,
                             const char *firmware_version, uint64_t session_id,
                             kvm_transport_send_fn send, void *send_context);

/** Clears partial frames and disarms the session after USB disconnect/reset. */
void kvm_transport_core_reset(kvm_transport_core_t *core);

/** Feeds arbitrarily split or coalesced USB bytes into the bounded frame parser. */
void kvm_transport_core_feed(kvm_transport_core_t *core, const uint8_t *bytes, size_t length);

/** Reports whether a validated SESSION_OPEN has been acknowledged. */
bool kvm_transport_core_session_open(const kvm_transport_core_t *core);
/** Binds the one serialized router and monotonic clock before USB input. */
void kvm_transport_core_bind_router(kvm_transport_core_t *core, kvm_router_t *router,
                                    kvm_transport_now_fn now, void *now_context);
/** Drains router input and enforces the lease while USB is idle. */
void kvm_transport_core_tick(kvm_transport_core_t *core);

#endif /* ESP32_KVM_TRANSPORT_CORE_H */
