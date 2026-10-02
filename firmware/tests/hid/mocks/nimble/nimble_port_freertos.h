/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares NimBLE's FreeRTOS task hooks for host-only lifecycle tests. */
#ifndef TEST_NIMBLE_FREERTOS_H
#define TEST_NIMBLE_FREERTOS_H
void nimble_port_freertos_init(void (*task)(void *));
void nimble_port_freertos_deinit(void);
#endif
