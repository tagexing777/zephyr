/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_PERIODIC_TASKS_CAN_LOOPBACK_H_
#define S32G_PERIODIC_TASKS_CAN_LOOPBACK_H_

#include <stdint.h>

/* Bounded CAN0 internal loopback; no pinmux or transceiver configuration. */
int32_t sample_can_loopback_test(void);

#endif /* S32G_PERIODIC_TASKS_CAN_LOOPBACK_H_ */
