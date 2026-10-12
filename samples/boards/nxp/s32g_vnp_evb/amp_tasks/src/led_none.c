/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>

#include "led.h"

int32_t sample_amp_led_initialize(void)
{
	return 0;
}

void sample_amp_led_tick(uint32_t elapsed)
{
	ARG_UNUSED(elapsed);
}
