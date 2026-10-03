/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Defines the single-threaded firmware routing actor. The USB transport calls
 * this API after frame validation; output callbacks target one of up to three
 * gated HID slots while stale sessions and generations cannot emit reports. */
#ifndef ESP32_KVM_ROUTER_H
#define ESP32_KVM_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Firmware lease interval measured from the last valid heartbeat. */
#define KVM_ROUTER_LEASE_MS 500u
/** Maximum age of relative motion when drained to an output. */
#define KVM_ROUTER_MOTION_AGE_MS 50u
/** Maximum continuous output backpressure before fail-local. */
#define KVM_ROUTER_CONGESTION_MS 100u
/** Bound on pending input events from the serialized actor. */
#define KVM_ROUTER_QUEUE_CAPACITY 8u
/** Maximum source-side HID slots; advertised capacity remains gated at one. */
#define KVM_ROUTER_MAX_SLOTS 3u

/** Protocol-compatible command outcomes. */
typedef enum {
    KVM_ROUTER_OK = 0,
    KVM_ROUTER_NOT_READY = 2,
    KVM_ROUTER_STALE_SESSION = 3,
    KVM_ROUTER_STALE_ROUTE = 4,
    KVM_ROUTER_PAUSED = 5,
    KVM_ROUTER_BAD_PAYLOAD = 6,
    KVM_ROUTER_BUSY = 7
} kvm_router_result_t;

/** Input report kind delivered to a selected output. */
typedef enum {
    KVM_ROUTER_KEYBOARD,
    KVM_ROUTER_POINTER,
    KVM_ROUTER_CONSUMER
} kvm_router_input_kind_t;

/** One full keyboard, pointer, or consumer state; pointer deltas are signed. */
typedef struct {
    kvm_router_input_kind_t kind;
    uint8_t keyboard[8];
    uint8_t buttons;
    int16_t dx, dy;
    int8_t wheel, pan;
    uint16_t consumer;
} kvm_router_input_t;

/** Output operations; release disarms first and returns false if delivery is uncertain. */
typedef struct {
    bool (*ready)(void *context, uint8_t slot);
    bool (*release)(void *context, uint8_t slot);
    bool (*arm)(void *context, uint8_t slot);
    bool (*send)(void *context, uint8_t slot, const kvm_router_input_t *input);
    void (*disconnect)(void *context, uint8_t slot);
} kvm_router_output_t;

/** Generation-tagged input entry, held only until the actor drains it. */
typedef struct {
    kvm_router_input_t input;
    uint32_t generation;
    uint64_t received_ms;
    uint32_t seq;
} kvm_router_entry_t;

/** Cached control result makes a same-session, same-sequence retry idempotent. */
typedef struct {
    uint32_t seq;
    uint8_t kind;
    uint32_t generation;
    uint64_t argument;
    uint8_t slot;
    kvm_router_result_t result;
    bool valid;
} kvm_router_control_result_t;

/** Actor state is public for static allocation and status reporting; one caller serializes it. */
typedef struct {
    kvm_router_output_t output;
    void *context;
    uint64_t session;
    uint64_t last_heartbeat_ms;
    uint64_t last_host_tick;
    uint64_t congestion_since_ms;
    uint32_t generation;
    uint32_t last_result_generation;
    uint8_t slot;
    uint8_t capacity;
    bool session_open;
    bool has_host_tick;
    bool armed;
    bool fault;
    bool congested;
    uint8_t queued;
    kvm_router_entry_t queue[KVM_ROUTER_QUEUE_CAPACITY];
    kvm_router_control_result_t controls[8];
    uint8_t next_control;
    uint32_t newest_control_seq;
    uint32_t newest_input_seq;
    /** Last input sequence whose output callback accepted a BLE notification. */
    uint32_t last_enqueued_input_seq;
    bool has_control_seq;
    bool has_input_seq;
    /** Distinguishes no enqueue yet from a valid sequence number zero. */
    bool has_enqueued_input_seq;
} kvm_router_t;

/** Initializes the router disarmed on local slot zero. */
void kvm_router_init(kvm_router_t *router, kvm_router_output_t output, void *context);
/** Sets an admitted slot capacity only before opening a session. Defaults to one. */
bool kvm_router_set_capacity(kvm_router_t *router, uint8_t capacity);
/** Opens a fresh transport session, releasing any old output. */
void kvm_router_session_open(kvm_router_t *router, uint64_t session, uint64_t now_ms);
/** Handles USB disconnect or board reset and returns to disarmed local state. */
void kvm_router_reset(kvm_router_t *router);
/** Releases the selected output after a physical emergency, closing the host session. */
void kvm_router_emergency_release(kvm_router_t *router);
/** Refreshes the lease only for the active session and a monotonic host tick. */
kvm_router_result_t kvm_router_heartbeat(kvm_router_t *router, uint64_t session,
                                         uint64_t host_tick, uint64_t now_ms);
/** Releases old output, installs a fresh generation, and selects a target disarmed. */
kvm_router_result_t kvm_router_switch(kvm_router_t *router, uint64_t session, uint32_t seq,
                                      uint8_t slot, uint32_t expected, uint32_t requested,
                                      uint64_t now_ms);
/** Sends an all-up baseline and arms a ready target. */
kvm_router_result_t kvm_router_arm(kvm_router_t *router, uint64_t session, uint32_t seq,
                                   uint8_t slot, uint32_t generation, uint64_t now_ms);
/** Releases held reports, disarms, and advances the generation in a valid session. */
kvm_router_result_t kvm_router_release_all(kvm_router_t *router, uint64_t session,
                                           uint32_t seq, uint64_t now_ms);
/** Queues one current-generation report; overflow fails local immediately. */
kvm_router_result_t kvm_router_input(kvm_router_t *router, uint64_t session,
                                     uint32_t generation, uint32_t seq,
                                     kvm_router_input_t input, uint64_t now_ms);
/** Drains reports and checks lease/congestion; call regularly from the actor loop. */
void kvm_router_tick(kvm_router_t *router, uint64_t now_ms);

#endif
