/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include "led.h"
#include "periodic_logic.h"

atomic_t m7_1_led_toggles;
int32_t m7_1_led_error;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static uint32_t led_phase;

int32_t sample_amp_led_initialize(void)
{
	int32_t err;

	if (!gpio_is_ready_dt(&led)) {
		m7_1_led_error = -ENODEV;
		return m7_1_led_error;
	}
	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	m7_1_led_error = err;
	return err;
}

void sample_amp_led_tick(uint32_t elapsed)
{
	if (m7_1_led_error != 0) {
		return;
	}
	if (sample_led_advance(&led_phase, elapsed)) {
		int32_t err = gpio_pin_toggle_dt(&led);

		if (err != 0) {
			m7_1_led_error = err;
			return;
		}
		atomic_inc(&m7_1_led_toggles);
	}
}
