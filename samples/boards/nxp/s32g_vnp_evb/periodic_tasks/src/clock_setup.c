/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "clock.h"

#define CGM0_BASE       0x40030000U
#define CORE_PLL_BASE   0x40038000U
#define CORE_DFS_BASE   0x40054000U
#define FXOSC_STAT      0x40050004U
#define MC_ME_BASE      0x40088000U
#define POLL_LIMIT      100000U
#define MUX_SOURCE_MASK GENMASK(29, 24)
#define MUX_SWITCH_BUSY BIT(16)
#define MUX_SWITCH_OK   BIT(17)
#define PLL_DIV_400MHZ  0x2050U
#define DFS_DIV_400MHZ  0x100U
#define DFS_PORTS_MASK  0x3fU

static int32_t wait_register(uintptr_t address, uint32_t mask, uint32_t expected)
{
	/* SysTick does not exist yet; bound register polls rather than using kernel time. */
	for (uint32_t i = 0U; i < POLL_LIMIT; i++) {
		if ((sys_read32(address) & mask) == expected) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static bool uses_core_pll(uintptr_t css)
{
	uint32_t source = (sys_read32(css) & MUX_SOURCE_MASK) >> 24U;

	return (source == 4U) || (source == 5U) || ((source >= 12U) && (source <= 17U));
}

static int32_t check_ownership(void)
{
	static const uint32_t other_core_status[] = {
		0x168U, 0x188U, 0x1a8U, 0x348U, 0x368U, 0x388U, 0x3a8U,
	};
	static const uint32_t other_mux_status[] = {
		0x344U, 0x384U, 0x3c4U, 0x404U, 0x444U, 0x484U, 0x4c4U,
		0x504U, 0x544U, 0x584U, 0x5c4U, 0x604U, 0x684U, 0x6c4U, 0x704U,
	};

	for (size_t i = 0U; i < ARRAY_SIZE(other_core_status); i++) {
		if ((sys_read32(MC_ME_BASE + other_core_status[i]) & BIT(0)) != 0U) {
			return -EBUSY;
		}
	}
	if (((sys_read32(CORE_PLL_BASE + 0x80U) |
	      sys_read32(CORE_PLL_BASE + 0x84U)) & BIT(31)) != 0U) {
		return -EBUSY;
	}
	if (((sys_read32(CORE_DFS_BASE + 0x0cU) & DFS_PORTS_MASK & ~BIT(0)) != 0U) ||
	    ((sys_read32(CORE_DFS_BASE + 0x14U) & DFS_PORTS_MASK & ~BIT(0)) !=
	     (DFS_PORTS_MASK & ~BIT(0)))) {
		return -EBUSY;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(other_mux_status); i++) {
		if (uses_core_pll(CGM0_BASE + other_mux_status[i])) {
			return -EBUSY;
		}
	}
	if (uses_core_pll(0x40034304U) || uses_core_pll(0x40068304U)) {
		return -EBUSY;
	}
	/* Partition 1 clock registers are accessible only when that partition is on. */
	if ((sys_read32(MC_ME_BASE + 0x308U) & BIT(0)) != 0U) {
		for (uint32_t i = 0U; i < 10U; i++) {
			if (uses_core_pll(0x44018304U + i * 0x40U)) {
				return -EBUSY;
			}
		}
	}
	return 0;
}

static int32_t select_xbar_source(uint32_t source)
{
	uint32_t value;
	int32_t ret = wait_register(CGM0_BASE + 0x304U, MUX_SWITCH_BUSY, 0U);

	if (ret != 0) {
		return ret;
	}
	value = sys_read32(CGM0_BASE + 0x300U);
	value &= ~(MUX_SOURCE_MASK | GENMASK(3, 0));
	value |= (source << 24U) | BIT(2);
	barrier_dsync_fence_full();
	sys_write32(value, CGM0_BASE + 0x300U);
	ret = wait_register(CGM0_BASE + 0x304U,
			    MUX_SOURCE_MASK | MUX_SWITCH_BUSY | GENMASK(19, 17),
			    (source << 24U) | MUX_SWITCH_OK);
	barrier_dsync_fence_full();
	barrier_isync_fence_full();
	return ret;
}

int32_t sample_clock_configure_fxosc(void)
{
	uint32_t pll_control;
	int32_t ret;

	if ((sys_read32(FXOSC_STAT) & BIT(31)) == 0U) {
		return -ENODEV;
	}
	/* DFS master-reset initialization belongs to the boot clock owner. */
	if ((sys_read32(CORE_DFS_BASE + 0x18U) & BIT(1)) != 0U) {
		return -ENOTSUP;
	}
	ret = check_ownership();
	if (ret != 0) {
		return ret;
	}
	/* Leave a matching warm-boot clock tree untouched. */
	if (((sys_read32(CORE_PLL_BASE + 0x20U) & 1U) == 1U) &&
	    ((sys_read32(CORE_PLL_BASE) & BIT(31)) == 0U) &&
	    ((sys_read32(CORE_PLL_BASE + 0x04U) & BIT(2)) != 0U) &&
	    ((sys_read32(CORE_PLL_BASE + 0x08U) & 0x70ffU) == PLL_DIV_400MHZ) &&
	    ((sys_read32(CORE_PLL_BASE + 0x10U) & (BIT(30) | 0x7fffU)) == 0U) &&
	    ((sys_read32(CORE_PLL_BASE + 0x0cU) & BIT(30)) != 0U) &&
	    ((sys_read32(CORE_DFS_BASE + 0x1cU) & 0xff3fU) == DFS_DIV_400MHZ) &&
	    ((sys_read32(CORE_DFS_BASE + 0x0cU) & BIT(0)) != 0U) &&
	    ((sys_read32(CGM0_BASE + 0x304U) & (MUX_SOURCE_MASK | MUX_SWITCH_BUSY)) ==
	     (12U << 24U))) {
		return 0;
	}

	/* Move XBAR/M7 off CORE_PLL before powering down its only enabled output. */
	ret = select_xbar_source(0U);
	if (ret != 0) {
		return ret;
	}
	sys_write32(sys_read32(CORE_DFS_BASE + 0x14U) | DFS_PORTS_MASK,
		    CORE_DFS_BASE + 0x14U);
	ret = wait_register(CORE_DFS_BASE + 0x0cU, DFS_PORTS_MASK, 0U);
	if (ret != 0) {
		return ret;
	}
	pll_control = sys_read32(CORE_PLL_BASE);
	sys_write32(pll_control | BIT(31), CORE_PLL_BASE);
	ret = wait_register(CORE_PLL_BASE + 0x04U, BIT(2), 0U);
	if (ret != 0) {
		return ret;
	}
	/* 40 MHz / 2 * 80 = 1600 MHz VCO; DFS1 / 2 and M7 / 2 yield 400 MHz. */
	sys_write32(1U, CORE_PLL_BASE + 0x20U);
	sys_write32(PLL_DIV_400MHZ, CORE_PLL_BASE + 0x08U);
	sys_write32(sys_read32(CORE_PLL_BASE + 0x10U) & ~(BIT(30) | 0x7fffU),
		    CORE_PLL_BASE + 0x10U);
	sys_write32(BIT(30), CORE_PLL_BASE + 0x0cU);
	sys_write32(pll_control & ~BIT(31), CORE_PLL_BASE);
	ret = wait_register(CORE_PLL_BASE + 0x04U, BIT(2), BIT(2));
	if (ret != 0) {
		return ret;
	}
	sys_write32(DFS_DIV_400MHZ, CORE_DFS_BASE + 0x1cU);
	sys_write32(sys_read32(CORE_DFS_BASE + 0x18U) & ~BIT(1), CORE_DFS_BASE + 0x18U);
	sys_write32(sys_read32(CORE_DFS_BASE + 0x14U) & ~BIT(0), CORE_DFS_BASE + 0x14U);
	ret = wait_register(CORE_DFS_BASE + 0x0cU, BIT(0), BIT(0));
	if (ret != 0) {
		return ret;
	}
	return select_xbar_source(12U);
}
