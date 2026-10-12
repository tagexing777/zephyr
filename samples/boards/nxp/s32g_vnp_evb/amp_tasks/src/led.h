/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_AMP_TASKS_LED_H_
#define S32G_AMP_TASKS_LED_H_

#include <stdint.h>

/**
 * @brief Initialize the core's optional LED before its periodic tasks start.
 *
 * @retval 0 LED configured, or this core owns no LED.
 * @retval -ENODEV M7_1 LED GPIO is not ready.
 * @return A negative GPIO driver error on configuration failure.
 */
int32_t sample_amp_led_initialize(void);

/**
 * @brief Advance the optional LED phase from the 1 ms thread.
 *
 * @param elapsed Number of elapsed 1 ms timer periods.
 */
void sample_amp_led_tick(uint32_t elapsed);

#endif /* S32G_AMP_TASKS_LED_H_ */
