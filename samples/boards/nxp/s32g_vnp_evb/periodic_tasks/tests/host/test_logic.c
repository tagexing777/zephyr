/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>

#include "../../src/clock.h"
#include "../../src/eeprom_read_test.h"
#include "../../src/periodic_logic.h"

static void test_clock(void)
{
	assert(sample_pll_hz(40000000U, 0x2050U, 0U, 4U) == 1600000000U);
	assert(sample_dfs_hz(1600000000U, 0x100U, 1U, 0U) / 2U == 400000000U);
	assert(sample_pll_hz(40000000U, 0x1028U, 0U, 4U) == 1600000000U);
	assert(sample_pll_hz(48000000U, 0x1032U, 0U, 4U) == 2400000000U);
	assert(sample_pll_hz(48000000U, 0x2028U, 0U, 4U) == 960000000U);
	assert(sample_pll_hz(40000000U, 0x0028U, 0U, 4U) == 1600000000U);
	assert(sample_pll_hz(48000000U, 0x1021U, 0x40001800U, 4U) == 1600000000U);
	assert(sample_pll_hz(40000000U, 0x1028U, 0U, 0U) == 0U);
	assert(sample_dfs_hz(1600000000U, 0x100U, 1U, 0U) == 800000000U);
	assert(sample_dfs_hz(1600000000U, 0x206U, 1U, 0U) == 369230769U);
	assert(sample_dfs_hz(1600000000U, 0x100U, 0U, 0U) == 0U);
	assert(sample_dfs_hz(1600000000U, 0U, 1U, 0U) == 0U);
	assert(sample_dfs_hz(1600000000U, 0x100U, 0x20U, 5U) == 800000000U);
	assert(sample_dfs_hz(1600000000U, 0x100U, UINT32_MAX, 32U) == 0U);
}

static void test_led(void)
{
	uint32_t phase = 0U;
	uint32_t toggles = 0U;

	for (uint32_t tick = 1U; tick <= 2000U; tick++) {
		bool toggle = sample_led_advance(&phase, 1U);

		assert(toggle == ((tick % 500U) == 0U));
		if (toggle) {
			toggles++;
		}
	}
	assert(toggles == 4U);
	assert(phase == 0U);
	assert(!sample_led_advance(&phase, 499U));
	assert(sample_led_advance(&phase, 1U));
	assert(!sample_led_advance(&phase, 1000U));
	assert(sample_led_advance(&phase, 1500U));
	assert(phase == 0U);
	phase = 499U;
	assert(sample_led_advance(&phase, UINT32_MAX));
	assert(phase == 294U);
	assert(!sample_led_advance(&phase, 0U));
}

static void test_eeprom_write_range(void)
{
	assert(!sample_eeprom_write_range_valid(0U, 1U));
	assert(!sample_eeprom_write_range_valid(119U, 1U));
	assert(!sample_eeprom_write_range_valid(120U, 0U));
	assert(sample_eeprom_write_range_valid(120U, 8U));
	assert(sample_eeprom_write_range_valid(121U, 7U));
	assert(sample_eeprom_write_range_valid(127U, 1U));
	assert(!sample_eeprom_write_range_valid(127U, 2U));
	assert(!sample_eeprom_write_range_valid(128U, 1U));
	assert(!sample_eeprom_write_range_valid(255U, 1U));
	assert(!sample_eeprom_write_range_valid(120U, SIZE_MAX));
}

int main(void)
{
	test_clock();
	test_led();
	test_eeprom_write_range();
	puts("Clock, LED phase and EEPROM write-boundary tests passed");
	return 0;
}
