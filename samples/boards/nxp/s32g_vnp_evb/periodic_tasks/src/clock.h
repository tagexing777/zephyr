/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_PERIODIC_TASKS_CLOCK_H_
#define S32G_PERIODIC_TASKS_CLOCK_H_

#include <stdint.h>

/* Startup result for main() and debugger inspection. Zero means unsupported. */
extern uint32_t sample_core_clock_hz;
extern uint32_t sample_clock_source;
extern int32_t sample_clock_error;

/**
 * @brief Configure the standalone M7 clock from the stable board crystal before SysTick.
 *
 * Requires exclusive ownership of CORE_PLL and XBAR clocks. Other cores must
 * remain disabled; state checks do not establish an AMP resource reservation.
 * After a failure during reconfiguration, XBAR may remain on FIRC. Do not start
 * application tasks when this function fails.
 *
 * @retval 0 Crystal-based 400 MHz clock configured or already selected.
 * @retval -ENODEV FXOSC is not stable.
 * @retval -ENOTSUP Boot firmware has not initialized the CORE_DFS master.
 * @retval -EBUSY Another core or clock consumer is enabled.
 * @retval -ETIMEDOUT Clock switch, PLL lock, or DFS status poll exceeded its bound.
 */
int32_t sample_clock_configure_fxosc(void);

static inline uint64_t sample_pll_hz(uint32_t ref_hz, uint32_t pll_div,
				    uint32_t pll_frac, uint32_t pll_status)
{
	uint32_t rdiv = (pll_div >> 12U) & 7U;
	uint32_t mfi = pll_div & 0xffU;

	if ((pll_status & 4U) == 0U) {
		return 0U;
	}
	if (rdiv == 0U) {
		rdiv = 1U;
	}

	/* NXP Clock_Ip: VCO = reference * (MFI + MFN / 18432) / RDIV. */
	return (uint64_t)ref_hz * ((uint64_t)mfi * 18432U + (pll_frac & 0x7fffU)) /
	       ((uint64_t)rdiv * 18432U);
}

static inline uint64_t sample_dfs_hz(uint64_t vco_hz, uint32_t div,
				    uint32_t status, uint32_t port)
{
	uint32_t denominator = 36U * ((div >> 8U) & 0xffU) + (div & 0x3fU);

	if ((port >= 6U) || ((status & (1U << port)) == 0U) || (denominator == 0U)) {
		return 0U;
	}
	return vco_hz * 18U / denominator;
}

#endif /* S32G_PERIODIC_TASKS_CLOCK_H_ */
