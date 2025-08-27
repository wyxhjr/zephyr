/*
 * Copyright (c) 2024 Zephyr contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_INTC_SHARED_H_
#define ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_INTC_SHARED_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shared external interrupt handler for APLIC and IMSIC
 * This function handles both APLIC and IMSIC interrupts on the same IRQ line
 * Implements true hardware signal line sharing
 */
void shared_ext_isr(const void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_INTERRUPT_CONTROLLER_INTC_SHARED_H_ */
