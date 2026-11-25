/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Runs the input hub beside the router on the USB worker. A bounded queue
 * carries device reports from the NimBLE host thread; the worker drains it and
 * is the only caller of the hub and the router. Arbitration: the hub routes
 * device input to its own target while the router is not armed (no host
 * session, or a session that has not armed a guest) and drops it while the
 * router is armed. The hub yields (all-up, release) at the moment the router
 * actually arms (after every router check passed), never refusing the host's
 * ARM, and forgets its arm when the router releases that slot. Device work is
 * bounded per worker iteration so host heartbeats are never starved. */
#ifndef ESP32_KVM_TRANSPORT_HUB_H
#define ESP32_KVM_TRANSPORT_HUB_H

#include "input_hub.h"
#include "router.h"
#include "transport_core.h"

/** Ring depth for device reports. A pending device loss and the overflow
 * latest-state slots live outside the ring, so neither is ever dropped. */
#define KVM_HUB_QUEUE_DEPTH 16u
/** First and largest delay between central start attempts. */
#define KVM_HUB_CENTRAL_BACKOFF_MIN_MS 100u
#define KVM_HUB_CENTRAL_BACKOFF_MAX_MS 10000u
/** Device work per worker iteration stops once this much time has passed. */
#define KVM_HUB_DRAIN_BUDGET_MS 20u

enum { KVM_HUB_MSG_INPUT = 1, KVM_HUB_MSG_LOST = 2 };

/** One device report (INPUT) or one device loss (LOST). For LOST only
 * input.kind is read: KVM_ROUTER_KEYBOARD is a lost keyboard (its keys and
 * consumer usage are released), KVM_ROUTER_POINTER a lost mouse (its buttons);
 * any other kind releases everything. */
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
 * always arrives and order is kept. A LOST message is a flag per lost kind
 * outside the ring, each delivered once, after the reports queued before it
 * and before any later ones; it supersedes only the tail states it releases. */
typedef struct {
    kvm_hub_msg_t ring[KVM_HUB_QUEUE_DEPTH];
    bool motion[KVM_HUB_QUEUE_DEPTH];
    uint8_t count;
    uint8_t lost_mask;      /* bit (1 << kind): keyboard, pointer, or 2 for a whole loss */
    uint8_t lost_after[3];  /* ring position each pending loss is delivered at */
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
/** True while any message (ring, tail state or loss) is waiting. */
bool kvm_hub_queue_pending(kvm_hub_queue_t *queue);

typedef uint64_t (*kvm_hub_clock_fn)(void *context);

/** Worker-side hub state; only the USB worker touches it and the router. */
typedef struct {
    kvm_input_hub_t hub;
    kvm_router_t *router;
    kvm_router_output_t output;
    void *context;
    kvm_hub_clock_fn now;
    void *now_context;
    bool select_pending;
    uint8_t select_target;
    kvm_hub_result_t last_result;
    uint32_t switch_failures;
    /** Hub-side output requests in the current drain, and whether one failed. */
    uint32_t requests;
    bool request_failed;
} kvm_hub_worker_t;

/** Binds the hub to the real HID output. The router must be initialized with
 * kvm_hub_worker_router_output() and this worker as its context. */
void kvm_hub_worker_init(kvm_hub_worker_t *worker, kvm_router_t *router,
                         kvm_router_output_t output, void *context);
/** Monotonic milliseconds for the drain budget; without it only the count bounds a drain. */
void kvm_hub_worker_set_clock(kvm_hub_worker_t *worker, kvm_hub_clock_fn now, void *context);
/** Router output that delegates to the worker's output. Its arm runs only when
 * kvm_router_arm has passed every check and is about to arm: the hub yields
 * there, and if that fails its channel is released directly (disconnected when
 * even that is uncertain) and abandoned, so the router's arm always proceeds.
 * An arm the channel refuses (it may still be armed by an arm nobody tracks)
 * is retried exactly once after releasing that slot, so a stale channel arm
 * never turns a host ARM into NOT_READY, which ends the host session.
 * Releasing or disconnecting the hub's slot makes the hub forget its arm. */
kvm_router_output_t kvm_hub_worker_router_output(void);
/** Applies one message: INPUT through the hub, LOST as a loss of that device
 * kind (kvm_input_hub_device_lost_kind). */
void kvm_hub_worker_message(kvm_hub_worker_t *worker, const kvm_hub_msg_t *msg);
/** First resends what a dropped or refused request left behind (a dirty hub,
 * kvm_input_hub_flush); while the armed slot is live but not answering it
 * returns at once, leaving every message queued and in order. Then retries a
 * pending selection and drains in delivery order, stopping at whichever comes
 * first: the queue is empty, KVM_HUB_QUEUE_DEPTH messages,
 * KVM_HUB_DRAIN_BUDGET_MS elapsed, or a failed hub output request.
 * Undelivered messages stay queued. */
void kvm_hub_worker_drain(kvm_hub_worker_t *worker, kvm_hub_queue_t *queue);
/** One worker iteration after the USB read: host bytes first (feed), then the
 * router tick (lease, queued host input), then the bounded device drain. A
 * NULL core skips the host part (USB disconnected). */
void kvm_hub_worker_iteration(kvm_hub_worker_t *worker, kvm_hub_queue_t *queue,
                              kvm_transport_core_t *core, const uint8_t *bytes,
                              size_t length);
/** Touch or PLUS selection. False when a host session owns selection; true when
 * the hub took the target or will retry it on the next device report. */
bool kvm_hub_worker_select(kvm_hub_worker_t *worker, uint8_t target);
/** A guest slot dropped (DISCONNECTED event): marks it known disconnected and
 * forgets the hub arm on that slot. 0 (slot unknown) releases the hub's arm
 * with all-up instead of assuming its link dropped. */
void kvm_hub_worker_slot_lost(kvm_hub_worker_t *worker, uint8_t slot);
/** A valid slot sample: bit (slot - 1) set for each slot whose HID host reports
 * it ready (connected, encrypted, subscribed). A cleared bit is the only thing
 * besides slot_lost that lets the hub forget an arm without a confirmed release
 * (kvm_input_hub_set_live); a false ready() alone (the request bridge can be
 * busy) never does. Call only with a sample that succeeded. */
void kvm_hub_worker_set_live(kvm_hub_worker_t *worker, uint8_t live_mask);
/** Physical emergency release: all-up and release the hub's arm. */
void kvm_hub_worker_release(kvm_hub_worker_t *worker);
/** Delay before central start attempt failures + 1, doubling and capped. */
uint32_t kvm_hub_central_backoff_ms(uint32_t failures);

#endif /* ESP32_KVM_TRANSPORT_HUB_H */
