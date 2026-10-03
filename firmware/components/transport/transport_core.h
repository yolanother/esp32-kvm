/* Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
 * Defines the disarmed firmware USB loopback protocol core. It accepts bounded
 * COBS frames, validates CRC32C and sessions, and dispatches authenticated
 * routing commands to a bound serialized router. Minor-one pairing controls
 * use bounded callbacks and STATUS events. Minor-two STATUS exposes up to
 * three cached authenticated guest tokens, while the admitted routing
 * capacity defaults to one. Retained inventory uses a bounded callback;
 * a USB CDC adapter owns I/O. */
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

/** One authenticated live HID slot sampled on the NimBLE host loop. */
typedef struct {
    uint8_t token[16];
    bool ready;
    bool subscribed;
} kvm_transport_slot_t;

/** Minor-one pairing states carried in STATUS key 6. */
typedef enum {
    KVM_PAIRING_CLOSED = 0, KVM_PAIRING_WAITING = 1,
    KVM_PAIRING_CHALLENGE = 2, KVM_PAIRING_REJECTED = 3,
    KVM_PAIRING_CAPACITY = 4, KVM_PAIRING_TIMEOUT = 5
} kvm_transport_pairing_state_t;
/** Bounded pairing commands execute on the NimBLE host loop. */
typedef struct {
    bool (*begin)(void *context);
    bool (*cancel)(void *context);
    bool (*reply)(void *context, uint32_t challenge_id, bool approved);
    /** Forgets only the explicitly supplied opaque token. */
    bool (*forget)(void *context, const uint8_t token[16]);
    /** Copies up to eight retained opaque tokens; false means unavailable. */
    bool (*inventory)(void *context, uint8_t tokens[8][16], uint8_t *count);
} kvm_transport_pairing_ops_t;

/** Private stream and handshake state for one USB connection. */
typedef struct {
    uint8_t rx[KVM_TRANSPORT_ENCODED_CAPACITY];
    size_t rx_length;
    bool draining;
    bool session_open;
    uint16_t minor;
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
    uint32_t next_select_request_id;
    kvm_transport_pairing_ops_t pairing_ops;
    void *pairing_context;
    kvm_transport_pairing_state_t pairing_state;
    uint32_t pairing_challenge_id;
    uint32_t pairing_number;
    uint64_t pairing_deadline_ms;
    kvm_transport_pairing_state_t status_pairing_state;
    uint32_t status_challenge_id;
    uint8_t connected_token[16];
    kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS];
    bool slots_dirty;
    /** Last BLE-enqueued input sequence already sent in INPUT_PROGRESS. */
    uint32_t reported_enqueued_input_seq;
    /** Distinguishes an unreported sequence from a valid sequence zero. */
    bool has_reported_enqueued_input_seq;
    bool last_forget_valid;
    uint32_t last_forget_seq;
    uint32_t last_forget_generation;
    uint8_t last_forget_token[16];
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
/** Binds serialized, bounded pairing operations from the USB worker. */
void kvm_transport_core_bind_pairing(kvm_transport_core_t *core,
                                     kvm_transport_pairing_ops_t ops, void *context);
/** Applies a queued NimBLE pairing event on the USB worker. */
void kvm_transport_core_pairing_event(kvm_transport_core_t *core,
                                      kvm_transport_pairing_state_t state,
                                      uint32_t challenge_id, uint32_t number,
                                      uint64_t deadline_ms);
/** Caches one nonzero authenticated connected-peer token; NULL clears it.
 * Only the USB worker calls this after a bounded HID host-loop lookup. */
void kvm_transport_core_set_connected_token(kvm_transport_core_t *core,
                                            const uint8_t token[16]);
/** Replaces all live slots atomically; rejects duplicate or inconsistent tokens. */
bool kvm_transport_core_set_slots(kvm_transport_core_t *core,
                                  const kvm_transport_slot_t slots[KVM_ROUTER_MAX_SLOTS]);
/** Drains router input and enforces the lease while USB is idle. */
void kvm_transport_core_tick(kvm_transport_core_t *core);
/** Sends an arbitration request for a physical PLUS press in an open USB session. */
bool kvm_transport_core_device_select_request(kvm_transport_core_t *core, uint8_t slot);

#endif /* ESP32_KVM_TRANSPORT_CORE_H */
