/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Runs the input hub beside the router on the USB worker. A bounded queue
 * carries device reports from the NimBLE host thread; the worker drains it and
 * is the only caller of the hub and the router. Arbitration: the hub routes
 * device input to its own target while the router is not armed (no host
 * session, or a session that has not armed a guest) and drops it while the
 * router is armed. The hub yields (all-up, release) before any host ARM reaches
 * the router, and forgets its arm when the router releases that slot. */
#ifndef ESP32_KVM_TRANSPORT_HUB_H
#define ESP32_KVM_TRANSPORT_HUB_H

#include "input_hub.h"
#include "router.h"

/** Ring depth for device reports. A pending device loss and the overflow
 * latest-state slots live outside the ring, so neither is ever dropped. */
#define KVM_HUB_QUEUE_DEPTH 16u
/** First and largest delay between central start attempts. */
#define KVM_HUB_CENTRAL_BACKOFF_MIN_MS 100u
#define KVM_HUB_CENTRAL_BACKOFF_MAX_MS 10000u

enum { KVM_HUB_MSG_INPUT = 1, KVM_HUB_MSG_LOST = 2 };

/** One device report (INPUT) or one device loss (LOST, input ignored). */
typedef struct {
    uint8_t type;
    kvm_router_input_t input;
} kvm_hub_msg_t;

typedef void (*kvm_hub_lock_fn)(void *context);

/** Multi-producer, single-consumer queue. Overflow policy: a pure pointer motion
 * (buttons unchanged) is dropped when the ring is full, and the oldest queued
 * motion is evicted to make room for anything else. With no motion to evict the
 * queue enters tail mode: every later message keeps only the newest state per
 * kind (pointer motion accumulates), so the final key, button and consumer state
 * always arrives and order is kept. A LOST message is a flag outside the ring,
 * delivered after the reports queued before it and before any later ones. */
typedef struct {
    kvm_hub_msg_t ring[KVM_HUB_QUEUE_DEPTH];
    bool motion[KVM_HUB_QUEUE_DEPTH];
    uint8_t count;
    bool lost;
    uint8_t lost_after;
    bool tail;
    bool tail_has[3];
    kvm_router_input_t tail_input[3];
    uint8_t last_buttons;
    uint32_t dropped_motion;
    kvm_hub_lock_fn lock;
    kvm_hub_lock_fn unlock;
    void *lock_context;
} kvm_hub_queue_t;

/** Initializes an empty queue; NULL lock functions mean single-threaded use. */
void kvm_hub_queue_init(kvm_hub_queue_t *queue, kvm_hub_lock_fn lock, kvm_hub_lock_fn unlock,
                        void *lock_context);
/** Copies one message in. Returns false only when a pure motion report was dropped. */
bool kvm_hub_queue_push(kvm_hub_queue_t *queue, const kvm_hub_msg_t *msg);
/** Takes the next message in delivery order; false when empty. */
bool kvm_hub_queue_pop(kvm_hub_queue_t *queue, kvm_hub_msg_t *out);

/** Worker-side hub state; only the USB worker touches it and the router. */
typedef struct {
    kvm_input_hub_t hub;
    kvm_router_t *router;
    kvm_router_output_t output;
    void *context;
    bool select_pending;
    uint8_t select_target;
    kvm_hub_result_t last_result;
    uint32_t switch_failures;
} kvm_hub_worker_t;

/** Binds the hub to the real HID output. The router must be initialized with
 * kvm_hub_worker_router_output() and this worker as its context. */
void kvm_hub_worker_init(kvm_hub_worker_t *worker, kvm_router_t *router,
                         kvm_router_output_t output, void *context);
/** Router output that delegates to the worker's output: arming first makes the
 * hub yield, and releasing or disconnecting the hub's slot forgets its arm. */
kvm_router_output_t kvm_hub_worker_router_output(void);
/** Transport ARM gate: yields the hub before a host ARM reaches the router.
 * False (the hub could not release a ready slot) refuses the ARM with BUSY. */
bool kvm_hub_worker_arm_gate(void *worker, uint8_t slot);
/** Applies one message: INPUT through the hub, LOST as a device loss. */
void kvm_hub_worker_message(kvm_hub_worker_t *worker, const kvm_hub_msg_t *msg);
/** Drains at most KVM_HUB_QUEUE_DEPTH messages so the worker stays responsive. */
void kvm_hub_worker_drain(kvm_hub_worker_t *worker, kvm_hub_queue_t *queue);
/** Touch or PLUS selection. False when a host session owns selection; true when
 * the hub took the target or will retry it on the next device report. */
bool kvm_hub_worker_select(kvm_hub_worker_t *worker, uint8_t target);
/** A guest slot dropped: forgets the hub arm on that slot. 0 (slot unknown)
 * releases the hub's arm with all-up instead of assuming its link dropped. */
void kvm_hub_worker_slot_lost(kvm_hub_worker_t *worker, uint8_t slot);
/** Physical emergency release: all-up and release the hub's arm. */
void kvm_hub_worker_release(kvm_hub_worker_t *worker);
/** Delay before central start attempt failures + 1, doubling and capped. */
uint32_t kvm_hub_central_backoff_ms(uint32_t failures);

#endif /* ESP32_KVM_TRANSPORT_HUB_H */
