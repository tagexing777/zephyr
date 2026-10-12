/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "clock.h"

#define MC_CGM0_MUX0_CSS 0x40030304U
#define CORE_PLL_BASE    0x40038000U
#define CORE_DFS_BASE    0x40054000U
#define FIRC_HZ          48000000U
#define FXOSC_HZ         40000000U

uint32_t sample_core_clock_hz;
uint32_t sample_clock_source;
int32_t sample_clock_error;

/* Kernel runtime frequency storage, initialized before the timer driver. */
extern unsigned int z_clock_hw_cycles_per_sec;

static uint64_t core_pll_hz(void)
{
	uint32_t pll_status = sys_read32(CORE_PLL_BASE + 0x04U);
	uint32_t pll_div = sys_read32(CORE_PLL_BASE + 0x08U);
	uint32_t pll_frac = sys_read32(CORE_PLL_BASE + 0x10U);
	uint32_t ref_select = sys_read32(CORE_PLL_BASE + 0x20U) & 1U;
	uint32_t ref_hz = (ref_select == 0U) ? FIRC_HZ : FXOSC_HZ;

	return sample_pll_hz(ref_hz, pll_div, pll_frac, pll_status);
}

static int sample_clock_init(void)
{
	uint64_t xbar_2x_hz = 0U;
	uint32_t source;

	if (IS_ENABLED(CONFIG_SAMPLE_S32G_CORE_PLL_FXOSC)) {
		sample_clock_error = sample_clock_configure_fxosc();
	}
	source = (sys_read32(MC_CGM0_MUX0_CSS) >> 24U) & 0x3fU;

	sample_clock_source = source;
	if (source == 0U) {
		xbar_2x_hz = FIRC_HZ;
	} else if (source == 2U) {
		xbar_2x_hz = FXOSC_HZ;
	} else if ((source == 4U) || (source == 5U)) {
		uint32_t odiv = sys_read32(CORE_PLL_BASE + 0x80U + (source - 4U) * 4U);

		if ((odiv & BIT(31)) != 0U) {
			xbar_2x_hz = core_pll_hz() / (((odiv >> 16U) & 0xffU) + 1U);
		}
	} else if ((source >= 12U) && (source <= 17U)) {
		uint32_t port = source - 12U;
		uint32_t status = sys_read32(CORE_DFS_BASE + 0x0cU);
		uint32_t div = sys_read32(CORE_DFS_BASE + 0x1cU + port * 4U);

		xbar_2x_hz = sample_dfs_hz(core_pll_hz(), div, status, port);
	}

	/* All three M7 cores run at XBAR_CLK, which is XBAR_2X_CLK / 2. */
	xbar_2x_hz /= 2U;
	if ((xbar_2x_hz < 1000U) || (xbar_2x_hz > 400000000U)) {
		/* Keep the bootstrap rate so main() can report the unsupported tree. */
		return 0;
	}

	sample_core_clock_hz = (uint32_t)xbar_2x_hz;
	/* Publish the resulting frequency before SysTick initialization. */
	z_clock_hw_cycles_per_sec = sample_core_clock_hz;
	return 0;
}

SYS_INIT(sample_clock_init, PRE_KERNEL_1, 0);
