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

#include "network.h"
#include "udp_log.h"

#define ECHO_PORT 4242U
#define LOG_PORT  4243U
#define LOG_HOST  "192.168.10.1"
#define LOG_LINK_SETTLE_MS 500U

uint32_t ethernet_echo_count;
uint32_t ethernet_echo_bytes;
int32_t sample_network_error;
atomic_t udp_log_task_count;
atomic_t udp_log_task_missed;
atomic_t udp_log_task_errors;
static atomic_t network_started;

K_TIMER_DEFINE(udp_log_timer_10ms, NULL, NULL);

static void task_udp_log_10ms(void *p1, void *p2, void *p3)
{
	struct net_if *iface = net_if_get_default();
	uint32_t stable_ms = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Let the link partner settle before the first ARP and log packet. */
	while (stable_ms < LOG_LINK_SETTLE_MS) {
		k_sleep(K_MSEC(50));
		if (net_if_is_carrier_ok(iface) && net_if_is_up(iface)) {
			stable_ms += 50U;
		} else {
			stable_ms = 0U;
		}
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

K_THREAD_DEFINE(udp_log_task_id, 2048, task_udp_log_10ms, NULL, NULL, NULL, 4, 0, -1);

static int32_t ethernet_run(void)
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

	if ((iface == NULL) || !device_is_ready(net_if_get_device(iface))) {
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
		return ret;
	}
	ret = udp_log_init(LOG_HOST, LOG_PORT);
	if (ret != 0) {
		return ret;
	}
	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}
	ret = zsock_bind(sock, (struct sockaddr *)&local, sizeof(local));
	if (ret != 0) {
		ret = -errno;
		goto close_socket;
	}
	k_thread_start(udp_log_task_id);
	printk("S32G M7_0 upper J108 GMAC0: 192.168.10.2 UDP %u\n", ECHO_PORT);
	printk("UDP log task started: 10 ms -> %s:%u\n", LOG_HOST, LOG_PORT);
	printk("UDP echo ready; awaiting PHY link and packets\n");
	while (true) {
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		ssize_t received;
		ssize_t sent;

		received = zsock_recvfrom(sock, buffer, sizeof(buffer), 0,
					 (struct sockaddr *)&peer, &peer_len);
		if (received < 0) {
			ret = -errno;
			break;
		}
		sent = zsock_sendto(sock, buffer, (size_t)received, 0,
				    (struct sockaddr *)&peer, peer_len);
		if (sent != received) {
			ret = (sent < 0) ? -errno : -EIO;
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
		printk("Echo socket close failed: %d\n", errno);
	}
	return ret;
}

static void task_ethernet(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	sample_network_error = ethernet_run();
	printk("Ethernet thread stopped: %d\n", sample_network_error);
}

K_THREAD_DEFINE(ethernet_task_id, 4096, task_ethernet, NULL, NULL, NULL, 6, 0, -1);

int32_t sample_network_start(void)
{
	int32_t ret;

	if (!atomic_cas(&network_started, 0, 1)) {
		return -EALREADY;
	}
	ret = k_thread_name_set(ethernet_task_id, "task_ethernet");
	if (ret == 0) {
		ret = k_thread_name_set(udp_log_task_id, "task_udp_log_10ms");
	}
	if (ret != 0) {
		atomic_set(&network_started, 0);
		return ret;
	}
	k_thread_start(ethernet_task_id);
	return 0;
}
