/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_s32g_gmac

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/sys/sys_io.h>
#include "eth_dwmac_priv.h"

LOG_MODULE_REGISTER(s32g_gmac, CONFIG_ETHERNET_LOG_LEVEL);

#define PERIPH_PLL_BASE 0x4003c000U
#define GMAC_CTRL_STS   0x4007c004U
#define GMAC_TX_CSC     0x40030580U
#define GMAC_TX_CSS     0x40030584U
#define GMAC_TX_DC      0x40030588U
#define GMAC_RX_CSC     0x400305c0U
#define GMAC_RX_CSS     0x400305c4U
#define PLL_POWER_DOWN BIT(31)
#define PLL_LOCK       BIT(2)
#define PHI5_DIVIDER   (PERIPH_PLL_BASE + 0x94U)
#define PHI5_125MHZ    (BIT(31) | (15U << 16U))
#define DATA_BUS_WIDTH 64
#define S32G_FIFO_SIZE 16384U

DWMAC_ASSERT_BUFFER_ALIGNMENT(DATA_BUS_WIDTH);
BUILD_ASSERT(!IS_ENABLED(CONFIG_DCACHE), "S32G GMAC bring-up requires uncached SRAM");
BUILD_ASSERT(DT_INST_ENUM_HAS_VALUE(0, phy_connection_type, rgmii),
	     "S32G GMAC driver supports RGMII only");

PINCTRL_DT_INST_DEFINE(0);

/* Keep every 16-byte descriptor within one AXI 4 KiB boundary. */
static struct dwmac_dma_desc tx_descs[NB_TX_DESCS] __aligned(16);
static struct dwmac_dma_desc rx_descs[NB_RX_DESCS] __aligned(16);

static int gmac_clock_select(uintptr_t control, uintptr_t status, uint32_t source)
{
	uint32_t value = sys_read32(control);

	value = (value & ~GENMASK(29, 24)) | (source << 24U) | BIT(2);
	sys_write32(value, control);
	if (!WAIT_FOR(((sys_read32(status) & BIT(16)) == 0U) &&
		      (((sys_read32(status) >> 24U) & 0x3fU) == source),
		      10000, k_busy_wait(1))) {
		return -ETIMEDOUT;
	}
	return 0;
}

static int gmac_clock_init(void)
{
	uint32_t control = sys_read32(PERIPH_PLL_BASE);
	uint32_t phi5 = sys_read32(PHI5_DIVIDER);
	int32_t ret;

	/* Never retune a shared, running PLL. The opt-in is for standalone M7 boot. */
	if ((control & PLL_POWER_DOWN) != 0U) {
		if (!DT_INST_PROP(0, nxp_initialize_periph_pll)) {
			return -ENODEV;
		}
		sys_write32(1U, PERIPH_PLL_BASE + 0x20U);
		sys_write32((sys_read32(PERIPH_PLL_BASE + 0x08U) & ~0x70ffU) | 0x1032U,
			    PERIPH_PLL_BASE + 0x08U);
		sys_write32(BIT(30), PERIPH_PLL_BASE + 0x0cU);
		sys_write32(0U, PERIPH_PLL_BASE + 0x10U);
		sys_write32(control & ~PLL_POWER_DOWN, PERIPH_PLL_BASE);
	}
	if (!WAIT_FOR((sys_read32(PERIPH_PLL_BASE + 0x04U) & PLL_LOCK) != 0U,
		      10000, k_busy_wait(1))) {
		return -ETIMEDOUT;
	}
	if (((sys_read32(PERIPH_PLL_BASE + 0x20U) & 1U) != 1U) ||
	    ((sys_read32(PERIPH_PLL_BASE + 0x08U) & 0x70ffU) != 0x1032U) ||
	    ((sys_read32(PERIPH_PLL_BASE + 0x10U) & 0x7fffU) != 0U)) {
		LOG_ERR("PERIPH PLL is not the required 2 GHz FXOSC configuration");
		return -EINVAL;
	}
	if (((phi5 & BIT(31)) != 0U) && (phi5 != PHI5_125MHZ)) {
		LOG_ERR("PERIPH PLL PHI5 is already configured differently");
		return -EBUSY;
	}
	sys_write32(PHI5_125MHZ, PHI5_DIVIDER);
	sys_write32(BIT(31), GMAC_TX_DC);
	ret = gmac_clock_select(GMAC_TX_CSC, GMAC_TX_CSS, 23U);
	if (ret != 0) {
		return ret;
	}
	/* RGMII receive clock comes from the selected PHY pad, not a shared PLL. */
	return gmac_clock_select(GMAC_RX_CSC, GMAC_RX_CSS, 38U);
}

int dwmac_bus_init(const struct device *dev)
{
	int32_t ret;

	ARG_UNUSED(dev);
	ret = pinctrl_apply_state(PINCTRL_DT_INST_DEV_CONFIG_GET(0), PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}
	/* Select RGMII without touching the unrelated FTM routing bits. */
	sys_write32((sys_read32(GMAC_CTRL_STS) & ~0x0fU) | 2U, GMAC_CTRL_STS);
	return gmac_clock_init();
}

void dwmac_platform_link_speed_changed(const struct device *dev, enum phy_link_speed speed)
{
	uint32_t divider;

	ARG_UNUSED(dev);
	switch (speed) {
	case LINK_HALF_10BASE:
	case LINK_FULL_10BASE:
		divider = 49U;
		break;
	case LINK_HALF_100BASE:
	case LINK_FULL_100BASE:
		divider = 4U;
		break;
	case LINK_HALF_1000BASE:
	case LINK_FULL_1000BASE:
		divider = 0U;
		break;
	default:
		LOG_ERR("Unsupported PHY speed %u", (uint32_t)speed);
		return;
	}
	sys_write32(BIT(31) | (divider << 16U), GMAC_TX_DC);
}

#define S32G_IRQ_CONNECT(name)                                                              \
	do {                                                                                  \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(0, name, irq),                                  \
			    DT_INST_IRQ_BY_NAME(0, name, priority), dwmac_isr,                  \
			    DEVICE_DT_INST_GET(0), 0);                                          \
		irq_enable(DT_INST_IRQ_BY_NAME(0, name, irq));                                  \
	} while (0)

int dwmac_platform_init(const struct device *dev)
{
	const struct net_eth_mac_config mac_cfg = NET_ETH_MAC_DT_INST_CONFIG_INIT(0);
	struct dwmac_priv *data = dev->data;

	data->tx_descs = tx_descs;
	data->rx_descs = rx_descs;
	DWMAC_REG_WRITE(MAC_CONF, MAC_CONF_DM);
	/* AXI supports INCR bursts; do not force the single-beat AHB configuration. */
	DWMAC_REG_WRITE(DMA_SYSBUS_MODE, DMA_SYSBUS_MODE_AAL | DMA_SYSBUS_MODE_BLEN32 |
			FIELD_PREP(DMA_SYSBUS_MODE_RD_OSR_LMT, 1U));
	S32G_IRQ_CONNECT(common);
	S32G_IRQ_CONNECT(tx);
	S32G_IRQ_CONNECT(rx);
	return net_eth_mac_load(&mac_cfg, data->mac_addr);
}

static const struct dwmac_config config = {
	DEVICE_MMIO_ROM_INIT(DT_DRV_INST(0)),
	.phy_dev = DEVICE_DT_GET(DT_INST_PHANDLE(0, phy_handle)),
	/* S32G2 has 16 KiB usable per direction despite the 32 KiB feature encoding. */
	.rx_fifo_size = S32G_FIFO_SIZE,
	.tx_fifo_size = S32G_FIFO_SIZE,
};

static struct dwmac_priv data;

ETH_NET_DEVICE_DT_INST_DEFINE(0, dwmac_probe, NULL, &data, &config,
			     CONFIG_ETH_INIT_PRIORITY, &dwmac_api, NET_ETH_MTU);
