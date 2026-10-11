/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "udp_log.h"

struct udp_log_message {
	uint16_t length;
	char text[UDP_LOG_MAX_PAYLOAD + 1U];
};

K_MSGQ_DEFINE(udp_log_queue, sizeof(struct udp_log_message), UDP_LOG_QUEUE_DEPTH, 4);
static struct sockaddr_in log_peer;
static int32_t log_socket = -1;
static atomic_t initialized;
static atomic_t queued_count;
static atomic_t sent_count;
static atomic_t dropped_count;
static atomic_t error_count;

static void udp_log_sender(void *p1, void *p2, void *p3)
{
	struct udp_log_message message;
	int32_t ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (true) {
		ssize_t sent;

		ret = k_msgq_get(&udp_log_queue, &message, K_FOREVER);
		if (ret != 0) {
			continue;
		}
		sent = zsock_sendto(log_socket, message.text, message.length,
				    ZSOCK_MSG_DONTWAIT,
				    (const struct sockaddr *)&log_peer, sizeof(log_peer));

		if (sent != (ssize_t)message.length) {
			atomic_inc(&error_count);
			atomic_inc(&dropped_count);
		} else {
			atomic_inc(&sent_count);
		}
	}
}

K_THREAD_DEFINE(udp_log_sender_id, 2048, udp_log_sender, NULL, NULL, NULL,
		5, 0, -1);

int32_t udp_log_init(const char *address, uint16_t port)
{
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	int32_t ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if ((address == NULL) || (port == 0U)) {
		return -EINVAL;
	}
	if (atomic_get(&initialized) != 0) {
		return -EALREADY;
	}
	ret = zsock_inet_pton(AF_INET, address, &log_peer.sin_addr);
	if (ret != 1) {
		return -EINVAL;
	}
	log_peer.sin_family = AF_INET;
	log_peer.sin_port = htons(port);
	log_socket = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (log_socket < 0) {
		return -errno;
	}
	ret = zsock_bind(log_socket, (struct sockaddr *)&local, sizeof(local));
	if (ret != 0) {
		ret = -errno;
		if (zsock_close(log_socket) != 0) {
			printk("UDP log socket close failed: %d\n", errno);
		}
		log_socket = -1;
		return ret;
	}
	atomic_set(&initialized, 1);
	k_thread_start(udp_log_sender_id);
	return 0;
}

int32_t udp_log_printf(const char *format, ...)
{
	struct udp_log_message message = {0};
	va_list args;
	int32_t length;
	int32_t ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (format == NULL) {
		return -EINVAL;
	}
	if (atomic_get(&initialized) == 0) {
		return -ENODEV;
	}
	va_start(args, format);
	length = vsnprintk(message.text, sizeof(message.text), format, args);
	va_end(args);
	if (length < 0) {
		return -EINVAL;
	}
	if ((uint32_t)length > UDP_LOG_MAX_PAYLOAD) {
		atomic_inc(&dropped_count);
		return -EMSGSIZE;
	}
	message.length = (uint16_t)length;
	ret = k_msgq_put(&udp_log_queue, &message, K_NO_WAIT);
	if (ret != 0) {
		atomic_inc(&dropped_count);
		return -ENOBUFS;
	}
	atomic_inc(&queued_count);
	return 0;
}

int32_t udp_log_get_stats(struct udp_log_stats *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}
	stats->queued = (uint32_t)atomic_get(&queued_count);
	stats->sent = (uint32_t)atomic_get(&sent_count);
	stats->dropped = (uint32_t)atomic_get(&dropped_count);
	stats->errors = (uint32_t)atomic_get(&error_count);
	return 0;
}
