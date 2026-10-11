/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_SAMPLE_UDP_LOG_H_
#define S32G_SAMPLE_UDP_LOG_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

/** Maximum formatted payload, excluding the terminating NUL byte. */
#define UDP_LOG_MAX_PAYLOAD 255U

/** Queue capacity, covering 1.28 seconds at the sample's 100 Hz rate. */
#define UDP_LOG_QUEUE_DEPTH 128U

/** Cumulative counters; fields are sampled independently. */
struct udp_log_stats {
	uint32_t queued;  /**< Messages accepted by the queue. */
	uint32_t sent;    /**< Datagrams accepted by the local network stack. */
	uint32_t dropped; /**< Messages rejected by size, queue, or transport limits. */
	uint32_t errors;  /**< Transport send failures. */
	uint32_t max_send_ms; /**< Longest socket send duration in target milliseconds. */
	int32_t last_send_error; /**< Last negative transport errno, or zero before an error. */
};

/**
 * @brief Initialize the sample UDP logger once from a thread.
 *
 * Call before starting producer threads. The destination is copied, and
 * initialization must not run concurrently. The socket remains open for the
 * application's lifetime. UDP does not acknowledge delivery to the receiver.
 *
 * @param address NUL-terminated numeric IPv4 destination, not NULL.
 * @param port Destination and local source port, in host order, from 1 to 65535.
 * @retval 0 Logger initialized.
 * @retval -EINVAL Invalid address or port.
 * @retval -EALREADY Logger already initialized.
 * @retval -EWOULDBLOCK Called from interrupt context.
 * @return Negative socket errno on socket or bind failure.
 */
int32_t udp_log_init(const char *address, uint16_t port);

/**
 * @brief Queue one printf-formatted UDP log without waiting for network I/O.
 *
 * Safe for concurrent thread callers, not interrupt context. Text is copied
 * into a static UDP_LOG_QUEUE_DEPTH-entry queue. Each accepted message becomes one datagram,
 * without the terminating NUL. No newline is added. Oversized messages are
 * rejected, not truncated. Network failures drop the message without retries.
 *
 * @param format NUL-terminated printf format, not NULL.
 * @param ... Arguments matching the format; data need only survive the call.
 * @retval 0 Message queued, not necessarily delivered.
 * @retval -EINVAL NULL format or formatting failure.
 * @retval -ENODEV Logger not initialized.
 * @retval -EWOULDBLOCK Called from interrupt context.
 * @retval -EMSGSIZE Formatted payload exceeds UDP_LOG_MAX_PAYLOAD.
 * @retval -ENOBUFS Log queue full.
 */
__printf_like(1, 2) int32_t udp_log_printf(const char *format, ...);

/**
 * @brief Read the logger's independently sampled counters.
 * @param[out] stats Destination for counters, not NULL.
 * @retval 0 Counters copied.
 * @retval -EINVAL NULL destination.
 */
int32_t udp_log_get_stats(struct udp_log_stats *stats);

#endif /* S32G_SAMPLE_UDP_LOG_H_ */
