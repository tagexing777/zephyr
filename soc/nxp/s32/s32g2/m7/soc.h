/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_NXP_S32_S32G2_M7_SOC_H_
#define ZEPHYR_SOC_NXP_S32_S32G2_M7_SOC_H_

#define __CM7_REV             0x0001U
#define __MPU_PRESENT         1U
#define __NVIC_PRIO_BITS      4U
#define __Vendor_SysTickConfig 0U
#define __FPU_PRESENT         1U
#define __ICACHE_PRESENT      1U
#define __DCACHE_PRESENT      1U
#define __DTCM_PRESENT        1U

typedef enum IRQn {
	NonMaskableInt_IRQn = -14,
	HardFault_IRQn = -13,
	MemoryManagement_IRQn = -12,
	BusFault_IRQn = -11,
	UsageFault_IRQn = -10,
	SVCall_IRQn = -5,
	DebugMonitor_IRQn = -4,
	PendSV_IRQn = -2,
	SysTick_IRQn = -1,
	SWT0_IRQn = 14,
	STM0_IRQn = 24,
	LINFLEXD0_IRQn = 82,
} IRQn_Type;

#include <core_cm7.h>

#endif /* ZEPHYR_SOC_NXP_S32_S32G2_M7_SOC_H_ */
