/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_NXP_S32G274A_525BGA_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_NXP_S32G274A_525BGA_PINCTRL_H_

#include <zephyr/dt-bindings/pinctrl/nxp-siul2-pinctrl.h>

/* LINFlexD0 signals routed to the S32G-VNP-EVB USB UART. */
#define PC9_LINFLEXD0_TX  NXP_SIUL2_PINMUX(0, 0, 41, 1, 0, 0)
#define PC10_LINFLEXD0_RX NXP_SIUL2_PINMUX(0, 0, 42, 0, 0, 2)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_NXP_S32G274A_525BGA_PINCTRL_H_ */
