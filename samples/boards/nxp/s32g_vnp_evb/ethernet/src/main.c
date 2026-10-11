/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "udp_log.h"

#define ECHO_PORT 4242U
#define LOG_PORT  4243U
#define LOG_HOST  "192.168.10.1"

uint32_t ethernet_echo_count;
uint32_t ethernet_echo_bytes;
atomic_t udp_log_task_count;
atomic_t udp_log_task_missed;
atomic_t udp_log_task_errors;

K_TIMER_DEFINE(udp_log_timer_10ms, NULL, NULL);

static void task_udp_log_10ms(void *p1, void *p2, void *p3)
{
	struct net_if *iface = net_if_get_default();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Do not accumulate logs during the initial PHY negotiation. */
	while (!net_if_is_carrier_ok(iface)) {
		k_sleep(K_MSEC(100));
	}
	k_timer_start(&udp_log_timer_10ms, K_MSEC(10), K_MSEC(10));
	while (true) {
		uint32_t elapsed = k_timer_status_sync(&udp_log_timer_10ms);
		uint32_t count = (uint32_t)atomic_inc(&udp_log_task_count) + 1U;
		int32_t ret;

		if (elapsed > 1U) {
			atomic_add(&udp_log_task_missed, (atomic_val_t)(elapsed - 1U));
		}
		ret = udp_log_printf("%s count=%" PRIu32 " uptime_ms=%" PRIu32
				     " missed=%" PRIu32 "\n", __func__, count, k_uptime_get_32(),
				     (uint32_t)atomic_get(&udp_log_task_missed));
		if (ret != 0) {
			atomic_inc(&udp_log_task_errors);
		}
	}
}

K_THREAD_DEFINE(udp_log_task_id, 2048, task_udp_log_10ms, NULL, NULL, NULL,
		4, 0, -1);

int main(void)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr address;
	struct in_addr netmask;
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = htons(ECHO_PORT),
	};
	uint8_t buffer[1472];
	int32_t sock;
	int32_t ret;

	printk("S32G M7_0 upper J108 GMAC0: 192.168.10.2 UDP %u\n", ECHO_PORT);
	if ((iface == NULL) || !device_is_ready(net_if_get_device(iface))) {
		printk("GMAC device is not ready\n");
		return -ENODEV;
	}
	ret = zsock_inet_pton(AF_INET, "192.168.10.2", &address);
	if (ret != 1) {
		return -EINVAL;
	}
	ret = zsock_inet_pton(AF_INET, "255.255.255.0", &netmask);
	if (ret != 1) {
		return -EINVAL;
	}
	if ((net_if_ipv4_addr_add(iface, &address, NET_ADDR_MANUAL, 0U) == NULL) ||
	    !net_if_ipv4_set_netmask_by_addr(iface, &address, &netmask)) {
		return -ENOMEM;
	}
	ret = net_if_up(iface);
	if ((ret != 0) && (ret != -EALREADY)) {
		printk("Interface up failed: %d\n", ret);
		return ret;
	}
	ret = udp_log_init(LOG_HOST, LOG_PORT);
	if (ret != 0) {
		printk("UDP log init failed: %d\n", ret);
		return ret;
	}
	k_thread_start(udp_log_task_id);
	printk("UDP log task started: 10 ms -> %s:%u\n", LOG_HOST, LOG_PORT);
	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		printk("Socket failed: %d\n", errno);
		return -errno;
	}
	ret = zsock_bind(sock, (struct sockaddr *)&local, sizeof(local));
	if (ret != 0) {
		printk("Bind failed: %d\n", errno);
		ret = -errno;
		goto close_socket;
	}
	printk("UDP echo ready; awaiting PHY link and packets\n");
	while (true) {
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		ssize_t received;
		ssize_t sent;

		received = zsock_recvfrom(sock, buffer, sizeof(buffer), 0,
					 (struct sockaddr *)&peer, &peer_len);
		if (received < 0) {
			printk("Receive failed: %d\n", errno);
			ret = -errno;
			break;
		}
		sent = zsock_sendto(sock, buffer, (size_t)received, 0,
				    (struct sockaddr *)&peer, peer_len);
		if (sent != received) {
			printk("Echo failed: %d\n", errno);
			ret = -EIO;
			break;
		}
		ethernet_echo_count++;
		ethernet_echo_bytes += (uint32_t)received;
		if ((ethernet_echo_count % 100U) == 0U) {
			printk("UDP echo packets=%u bytes=%u\n",
			       ethernet_echo_count, ethernet_echo_bytes);
		}
	}

close_socket:
	if (zsock_close(sock) != 0) {
		printk("Close failed: %d\n", errno);
	}
	return ret;
}
