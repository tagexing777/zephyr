/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define S32G_SWT0_BASE 0x40100000U
#define S32G_SWT_CR    0x00U
#define S32G_SWT_SR    0x10U

#define S32G_SWT_CR_WEN BIT(0)
#define S32G_SWT_CR_SLK BIT(4)
#define S32G_SWT_CR_HLK BIT(5)

#define S32G_SWT_UNLOCK_KEY_1 0xC520U
#define S32G_SWT_UNLOCK_KEY_2 0xD928U

static void s32g_swt0_disable(void)
{
	uint32_t control = sys_read32(S32G_SWT0_BASE + S32G_SWT_CR);

	if ((control & S32G_SWT_CR_WEN) == 0U) {
		return;
	}

	if ((control & S32G_SWT_CR_HLK) != 0U) {
		return;
	}

	if ((control & S32G_SWT_CR_SLK) != 0U) {
		sys_write32(S32G_SWT_UNLOCK_KEY_1, S32G_SWT0_BASE + S32G_SWT_SR);
		sys_write32(S32G_SWT_UNLOCK_KEY_2, S32G_SWT0_BASE + S32G_SWT_SR);
	}

	sys_write32(control & ~S32G_SWT_CR_WEN, S32G_SWT0_BASE + S32G_SWT_CR);
}

void soc_early_init_hook(void)
{
	s32g_swt0_disable();
}
