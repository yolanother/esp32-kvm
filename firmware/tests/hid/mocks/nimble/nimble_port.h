/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares NimBLE host initialization and run hooks for lifecycle tests. */
#ifndef TEST_NIMBLE_PORT_H
#define TEST_NIMBLE_PORT_H
#include "esp_err.h"
esp_err_t nimble_port_init(void);
int nimble_port_deinit(void);
void nimble_port_run(void);
#endif
