/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Models the NimBLE host event queue used to cross from router task to BLE. */
#ifndef TEST_NIMBLE_NPL_H
#define TEST_NIMBLE_NPL_H
#include <stdint.h>
struct ble_npl_event { void (*callback)(struct ble_npl_event *); void *argument; };
struct ble_npl_eventq { struct ble_npl_event *pending; struct ble_npl_event *next; };
struct ble_npl_callout { void (*callback)(struct ble_npl_event *); };
struct ble_npl_mutex { int unused; };
struct ble_npl_sem { unsigned tokens; };
typedef uint32_t ble_npl_time_t;
#define BLE_NPL_OK 0
#define BLE_NPL_TIME_FOREVER 0xffffffffu
void ble_npl_event_init(struct ble_npl_event *event,
                        void (*callback)(struct ble_npl_event *), void *argument);
void ble_npl_eventq_put(struct ble_npl_eventq *queue, struct ble_npl_event *event);
void ble_npl_callout_init(struct ble_npl_callout *callout, struct ble_npl_eventq *queue,
                          void (*callback)(struct ble_npl_event *), void *argument);
int ble_npl_callout_reset(struct ble_npl_callout *callout, uint32_t ticks);
void ble_npl_callout_stop(struct ble_npl_callout *callout);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms);
ble_npl_time_t ble_npl_time_get(void);
int ble_npl_mutex_init(struct ble_npl_mutex *mutex);
int ble_npl_mutex_pend(struct ble_npl_mutex *mutex, ble_npl_time_t timeout);
int ble_npl_mutex_release(struct ble_npl_mutex *mutex);
int ble_npl_sem_init(struct ble_npl_sem *semaphore, uint16_t tokens);
int ble_npl_sem_pend(struct ble_npl_sem *semaphore, ble_npl_time_t timeout);
int ble_npl_sem_release(struct ble_npl_sem *semaphore);
#endif
