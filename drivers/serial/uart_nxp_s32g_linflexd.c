/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_s32g_linflexd

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define LINFLEXD_LINCR1          0x00U
#define LINFLEXD_LINSR           0x08U
#define LINFLEXD_UARTCR          0x10U
#define LINFLEXD_UARTSR          0x14U
#define LINFLEXD_LINFBRR         0x24U
#define LINFLEXD_LINIBRR         0x28U
#define LINFLEXD_BDRL            0x38U
#define LINFLEXD_BDRM            0x3CU

#define LINFLEXD_LINCR1_INIT     BIT(0)
#define LINFLEXD_LINCR1_SLEEP    BIT(1)
#define LINFLEXD_LINSR_LINS_MASK GENMASK(15, 12)
#define LINFLEXD_LINSR_INIT_MODE (1U << 12)

#define LINFLEXD_UARTCR_UART BIT(0)
#define LINFLEXD_UARTCR_WL0  BIT(1)
#define LINFLEXD_UARTCR_TXEN BIT(4)
#define LINFLEXD_UARTCR_RXEN BIT(5)

#define LINFLEXD_UARTSR_DTF     BIT(1)
#define LINFLEXD_UARTSR_DRF     BIT(2)
#define LINFLEXD_UARTSR_PARITY  BIT(4)
#define LINFLEXD_UARTSR_NOISE   BIT(5)
#define LINFLEXD_UARTSR_OVERRUN BIT(7)
#define LINFLEXD_UARTSR_FRAMING BIT(8)
#define LINFLEXD_UARTSR_ERRORS  (LINFLEXD_UARTSR_PARITY | LINFLEXD_UARTSR_NOISE | \
				 LINFLEXD_UARTSR_OVERRUN | LINFLEXD_UARTSR_FRAMING)

#define LINFLEXD_INIT_TIMEOUT 1000000U

struct nxp_s32g_linflexd_config {
	DEVICE_MMIO_ROM;
	const struct pinctrl_dev_config *pincfg;
	uint32_t clock_frequency;
	uint32_t baud_rate;
};

static int nxp_s32g_linflexd_poll_in(const struct device *dev, unsigned char *c)
{
	mem_addr_t base = DEVICE_MMIO_GET(dev);
	uint32_t status;

	status = sys_read32(base + LINFLEXD_UARTSR);
	if ((status & LINFLEXD_UARTSR_DRF) == 0U) {
		return -1;
	}

	*c = (unsigned char)sys_read32(base + LINFLEXD_BDRM);
	sys_write32(LINFLEXD_UARTSR_DRF, base + LINFLEXD_UARTSR);

	return 0;
}

static void nxp_s32g_linflexd_poll_out(const struct device *dev, unsigned char c)
{
	mem_addr_t base = DEVICE_MMIO_GET(dev);

	sys_write32(LINFLEXD_UARTSR_DTF, base + LINFLEXD_UARTSR);
	sys_write32(c, base + LINFLEXD_BDRL);

	while ((sys_read32(base + LINFLEXD_UARTSR) & LINFLEXD_UARTSR_DTF) == 0U) {
		;
	}
}

static int nxp_s32g_linflexd_err_check(const struct device *dev)
{
	mem_addr_t base = DEVICE_MMIO_GET(dev);
	uint32_t status = sys_read32(base + LINFLEXD_UARTSR);
	uint32_t errors = status & LINFLEXD_UARTSR_ERRORS;
	int err = 0;

	if ((errors & LINFLEXD_UARTSR_OVERRUN) != 0U) {
		err |= UART_ERROR_OVERRUN;
	}
	if ((errors & LINFLEXD_UARTSR_PARITY) != 0U) {
		err |= UART_ERROR_PARITY;
	}
	if ((errors & LINFLEXD_UARTSR_FRAMING) != 0U) {
		err |= UART_ERROR_FRAMING;
	}
	if ((errors & LINFLEXD_UARTSR_NOISE) != 0U) {
		err |= UART_ERROR_NOISE;
	}

	sys_write32(errors, base + LINFLEXD_UARTSR);
	return err;
}

static int nxp_s32g_linflexd_init(const struct device *dev)
{
	const struct nxp_s32g_linflexd_config *config = dev->config;
	mem_addr_t base;
	uint32_t denominator;
	uint32_t integer_divisor;
	uint32_t fractional_divisor;
	uint32_t reg;
	uint32_t timeout;
	int err;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	base = DEVICE_MMIO_GET(dev);

	err = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		return err;
	}

	reg = sys_read32(base + LINFLEXD_LINCR1);
	reg &= ~LINFLEXD_LINCR1_SLEEP;
	reg |= LINFLEXD_LINCR1_INIT;
	sys_write32(reg, base + LINFLEXD_LINCR1);

	for (timeout = LINFLEXD_INIT_TIMEOUT; timeout > 0U; timeout--) {
		reg = sys_read32(base + LINFLEXD_LINSR);
		if ((reg & LINFLEXD_LINSR_LINS_MASK) == LINFLEXD_LINSR_INIT_MODE) {
			break;
		}
	}
	if (timeout == 0U) {
		return -EIO;
	}

	denominator = config->baud_rate * 16U;
	integer_divisor = config->clock_frequency / denominator;
	fractional_divisor = DIV_ROUND_CLOSEST((config->clock_frequency % denominator) * 16U,
					       denominator);
	if (fractional_divisor == 16U) {
		integer_divisor++;
		fractional_divisor = 0U;
	}
	if ((integer_divisor == 0U) || (integer_divisor > GENMASK(19, 0))) {
		return -EINVAL;
	}

	sys_write32(0U, base + LINFLEXD_UARTSR);
	sys_write32(integer_divisor, base + LINFLEXD_LINIBRR);
	sys_write32(fractional_divisor, base + LINFLEXD_LINFBRR);
	sys_write32(LINFLEXD_UARTCR_UART | LINFLEXD_UARTCR_WL0, base + LINFLEXD_UARTCR);

	reg = sys_read32(base + LINFLEXD_LINCR1);
	reg &= ~(LINFLEXD_LINCR1_SLEEP | LINFLEXD_LINCR1_INIT);
	sys_write32(reg, base + LINFLEXD_LINCR1);

	reg = sys_read32(base + LINFLEXD_UARTCR);
	reg |= LINFLEXD_UARTCR_TXEN | LINFLEXD_UARTCR_RXEN;
	sys_write32(reg, base + LINFLEXD_UARTCR);

	return 0;
}

static DEVICE_API(uart, nxp_s32g_linflexd_api) = {
	.poll_in = nxp_s32g_linflexd_poll_in,
	.poll_out = nxp_s32g_linflexd_poll_out,
	.err_check = nxp_s32g_linflexd_err_check,
};

#define NXP_S32G_LINFLEXD_INIT(inst)                                                        \
	PINCTRL_DT_INST_DEFINE(inst);                                                         \
	static const struct nxp_s32g_linflexd_config nxp_s32g_linflexd_config_##inst = {      \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                        \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                 \
		.clock_frequency = DT_INST_PROP(inst, clock_frequency),                         \
		.baud_rate = DT_INST_PROP(inst, current_speed),                                  \
	};                                                                                       \
	DEVICE_DT_INST_DEFINE(inst, nxp_s32g_linflexd_init, NULL, NULL,                         \
			      &nxp_s32g_linflexd_config_##inst, PRE_KERNEL_1,                   \
			      CONFIG_SERIAL_INIT_PRIORITY, &nxp_s32g_linflexd_api);

DT_INST_FOREACH_STATUS_OKAY(NXP_S32G_LINFLEXD_INIT)
