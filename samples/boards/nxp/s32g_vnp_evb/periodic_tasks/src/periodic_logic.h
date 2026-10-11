/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_PERIODIC_TASKS_LOGIC_H_
#define S32G_PERIODIC_TASKS_LOGIC_H_

#include <stdbool.h>
#include <stdint.h>

#define LED_TOGGLE_MS 500U

/* Advance by elapsed timer expirations; replay only the final LED state. */
static inline bool sample_led_advance(uint32_t *phase, uint32_t elapsed)
{
	uint64_t total = (uint64_t)*phase + elapsed;
	uint64_t transitions = total / LED_TOGGLE_MS;

	*phase = (uint32_t)(total % LED_TOGGLE_MS);
	return (transitions & 1U) != 0U;
}

#endif /* S32G_PERIODIC_TASKS_LOGIC_H_ */
