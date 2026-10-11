/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_SAMPLE_NETWORK_H_
#define S32G_SAMPLE_NETWORK_H_

#include <stdint.h>

/**
 * @brief Start the sample's asynchronous Ethernet initialization once.
 *
 * The existing periodic tasks continue independently of PHY negotiation.
 * This function does not wait for link, configure clocks, or stop other tasks.
 *
 * @retval 0 Network thread started, not necessarily linked.
 * @retval -EALREADY Network thread already started.
 * @return Negative kernel errno on thread-naming failure.
 */
int32_t sample_network_start(void);

#endif /* S32G_SAMPLE_NETWORK_H_ */
