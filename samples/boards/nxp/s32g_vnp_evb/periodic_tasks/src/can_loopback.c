/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "can_loopback.h"

#define CAN0_BASE 0x401b4000U
#define CAN_CLOCK_STATUS 0x400304c4U
#define FXOSC_STATUS 0x40050004U
#define MCR 0x00U
#define CTRL1 0x04U
#define TIMER 0x08U
#define ECR 0x1cU
#define ESR1 0x20U
#define IMASK1 0x28U
#define IFLAG1 0x30U
#define CTRL2 0x34U
#define MB_BASE 0x80U
#define RXIMR_BASE 0x880U
#define ERRSR 0xafcU
#define MCR_MDIS BIT(31)
#define MCR_FRZ BIT(30)
#define MCR_HALT BIT(28)
#define MCR_NOTRDY BIT(27)
#define MCR_SOFTRST BIT(25)
#define MCR_FRZACK BIT(24)
#define MCR_LPMACK BIT(20)
#define MCR_IRMQ BIT(16)
#define CTRL1_LPB BIT(12)
#define LOOPBACK_TIMING ((3U << 24U) | (1U << 22U) | (6U << 19U) | \
			(3U << 16U) | CTRL1_LPB | 7U)
#define CS_IDE BIT(21)
#define CS_SRR BIT(22)
#define CS_RTR BIT(20)
#define CS_CODE_MASK GENMASK(27, 24)
#define CS_DLC_MASK GENMASK(19, 16)
#define RX_EMPTY (4U << 24U)
#define RX_FULL (2U << 24U)
#define TX_INACTIVE (8U << 24U)
#define TX_DATA (12U << 24U)
#define RX_MB 1U
#define TX_MB 8U
#define MB_FLAGS (BIT(RX_MB) | BIT(TX_MB))
#define ERROR_MASK (GENMASK(15, 8) | GENMASK(5, 4) | BIT(2) | BIT(1))
#define FRAME_COUNT 1000U
#define TIMEOUT_MS 20U

struct can_loopback_result {
	uint32_t state;
	int32_t error;
	uint32_t stage;
	uint32_t transmitted;
	uint32_t received;
	uint32_t standard;
	uint32_t extended;
	uint32_t bytes;
	uint32_t mismatches;
	uint32_t timeouts;
	uint32_t clock_hz;
	uint32_t bitrate;
	uint32_t running_mcr;
	uint32_t running_ctrl1;
	uint32_t error_status;
	uint32_t error_count;
	int32_t cleanup_error;
	uint32_t final_mcr;
	uint32_t memory_errors;
	uint32_t dlc_seen;
};

volatile struct can_loopback_result sample_can_result;

static uint32_t read_reg(uint32_t offset)
{
	return sys_read32(CAN0_BASE + offset);
}

static void write_reg(uint32_t offset, uint32_t value)
{
	sys_write32(value, CAN0_BASE + offset);
	barrier_dsync_fence_full();
}

static int32_t wait_reg(uint32_t offset, uint32_t mask, uint32_t expected)
{
	int64_t deadline = k_uptime_get() + TIMEOUT_MS;

	do {
		if ((read_reg(offset) & mask) == expected) {
			return 0;
		}
	} while (k_uptime_get() < deadline);
	sample_can_result.timeouts++;
	return -ETIMEDOUT;
}

static int32_t check_available(void)
{
	static const uint32_t other_cores[] = {
		0x168U, 0x188U, 0x1a8U, 0x348U, 0x368U, 0x388U, 0x3a8U,
	};
	static const uint32_t interrupt_masks[] = {0x24U, 0x28U, 0x68U, 0x6cU};
	uint32_t clock = sys_read32(CAN_CLOCK_STATUS);
	uint32_t mcr = read_reg(MCR);

	/* Shared FLEXCAN_CLK is read, never switched. Only stable 40 MHz FXOSC is supported. */
	if (((clock & (GENMASK(28, 24) | BIT(16) | BIT(3))) != (2U << 24U)) ||
	    ((sys_read32(FXOSC_STATUS) & BIT(31)) == 0U)) {
		return -ENOTSUP;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(other_cores); i++) {
		if ((sys_read32(0x40088000U + other_cores[i]) & BIT(0)) != 0U) {
			return -EBUSY;
		}
	}
	for (size_t i = 0U; i < ARRAY_SIZE(interrupt_masks); i++) {
		if (read_reg(interrupt_masks[i]) != 0U) {
			return -EBUSY;
		}
	}
	if ((mcr & MCR_MDIS) == 0U) {
		/* Exclusive CAN0 ownership permits replacing idle boot receive buffers. */
		if (((read_reg(ESR1) & BIT(7)) == 0U) ||
		    ((read_reg(0x2cU) | read_reg(IFLAG1) | read_reg(0x70U) |
		      read_reg(0x74U)) != 0U)) {
			return -EBUSY;
		}
		for (uint32_t i = 0U; i <= (mcr & 0x7fU); i++) {
			uint32_t code = read_reg(MB_BASE + 16U * i) & CS_CODE_MASK;

			(void)read_reg(TIMER);
			if ((code != 0U) && (code != TX_INACTIVE) && (code != RX_EMPTY)) {
				return -EBUSY;
			}
		}
	}
	sample_can_result.clock_hz = 40000000U;
	sample_can_result.bitrate = 500000U;
	return 0;
}

static int32_t initialize(void)
{
	int32_t err;

	sample_can_result.stage = 2U;
	write_reg(MCR, (read_reg(MCR) | MCR_FRZ | MCR_HALT) & ~MCR_MDIS);
	err = wait_reg(MCR, MCR_FRZACK | MCR_LPMACK, MCR_FRZACK);
	if (err != 0) {
		return err;
	}
	sample_can_result.stage = 3U;
	write_reg(MCR, read_reg(MCR) | MCR_SOFTRST);
	err = wait_reg(MCR, MCR_SOFTRST, 0U);
	if (err != 0) {
		return err;
	}
	write_reg(MCR, MCR_FRZ | MCR_HALT | MCR_IRMQ | TX_MB);
	err = wait_reg(MCR, MCR_FRZACK, MCR_FRZACK);
	if (err != 0) {
		return err;
	}
	sample_can_result.stage = 4U;
	/* CBT and CTRL1 timing fields alias: clear CBT before writing CTRL1. */
	write_reg(0x50U, 0U);
	/* 40 MHz / 4 / 20 TQ = 500 kbit/s; sample point 80%, SJW 2 TQ. */
	write_reg(CTRL1, LOOPBACK_TIMING);
	write_reg(0xc00U, 0U); /* Eight-byte MB layout; CAN FD remains disabled. */
	write_reg(CTRL2, BIT(28)); /* WRMFRZ grants access to ECC-protected mask RAM. */
	/* S32G274A has 128 classic MBs: initialize all message and mask RAM ECC. */
	for (uint32_t offset = MB_BASE; offset < RXIMR_BASE; offset += 4U) {
		write_reg(offset, 0U);
	}
	for (uint32_t i = 0U; i < 128U; i++) {
		write_reg(RXIMR_BASE + i * 4U, UINT32_MAX);
	}
	write_reg(0x10U, UINT32_MAX);
	write_reg(0x14U, UINT32_MAX);
	write_reg(0x18U, UINT32_MAX);
	write_reg(0x48U, UINT32_MAX);
	write_reg(CTRL2, BIT(16) | BIT(17)); /* Compare IDE/RTR; do not answer remote frames. */
	write_reg(0x24U, 0U);
	write_reg(IMASK1, 0U);
	write_reg(0x68U, 0U);
	write_reg(0x6cU, 0U);
	write_reg(0x2cU, UINT32_MAX);
	write_reg(IFLAG1, UINT32_MAX);
	write_reg(0x70U, UINT32_MAX);
	write_reg(0x74U, UINT32_MAX);
	write_reg(MB_BASE + 16U * TX_MB, TX_INACTIVE);
	if (read_reg(CTRL1) != LOOPBACK_TIMING) {
		return -EIO;
	}
	sample_can_result.stage = 5U;
	/* SRXDIS=0 allows self reception; never leave freeze without LPB set. */
	write_reg(MCR, read_reg(MCR) & ~(MCR_FRZ | MCR_HALT));
	err = wait_reg(MCR, MCR_FRZACK | MCR_NOTRDY, 0U);
	sample_can_result.running_mcr = read_reg(MCR);
	sample_can_result.running_ctrl1 = read_reg(CTRL1);
	return err;
}

static int32_t transfer(uint32_t sequence)
{
	const uint32_t rx = MB_BASE + 16U * RX_MB;
	const uint32_t tx = MB_BASE + 16U * TX_MB;
	bool extended = (sequence % 2U) != 0U;
	uint32_t length = sequence % 9U;
	uint32_t id = (sequence * 7919U) & (extended ? 0x1fffffffU : 0x7ffU);
	uint32_t flags = extended ? CS_IDE | CS_SRR : 0U;
	uint32_t data[2] = {0U, 0U};
	uint32_t received_data[2];
	uint32_t received_cs;
	uint32_t received_id;
	int32_t err;

	if (sequence < 4U) {
		id = (sequence < 2U) ? 0U : (extended ? 0x1fffffffU : 0x7ffU);
	}
	if (!extended) {
		id <<= 18U;
	}
	for (uint32_t i = 0U; i < length; i++) {
		uint32_t byte = (sequence * 37U + i * 53U) & 0xffU;

		data[i / 4U] |= byte << (24U - 8U * (i % 4U));
	}
	write_reg(rx, 0U);
	write_reg(rx + 4U, id);
	write_reg(rx, RX_EMPTY | (flags & CS_IDE));
	write_reg(IFLAG1, MB_FLAGS);
	write_reg(tx, TX_INACTIVE);
	write_reg(tx + 4U, id);
	write_reg(tx + 8U, data[0]);
	write_reg(tx + 12U, data[1]);
	write_reg(tx, TX_DATA | flags | (length << 16U));
	err = wait_reg(IFLAG1, BIT(TX_MB), BIT(TX_MB));
	if (err != 0) {
		return err;
	}
	sample_can_result.transmitted++;
	err = wait_reg(IFLAG1, BIT(RX_MB), BIT(RX_MB));
	if (err != 0) {
		return err;
	}
	/* Reading CS locks the receive MB; TIMER releases the lock after the payload read. */
	received_cs = read_reg(rx);
	received_id = read_reg(rx + 4U);
	received_data[0] = read_reg(rx + 8U);
	received_data[1] = read_reg(rx + 12U);
	(void)read_reg(TIMER);
	write_reg(IFLAG1, MB_FLAGS);
	sample_can_result.received++;
	if (((received_cs & (CS_CODE_MASK | CS_IDE | CS_RTR | CS_DLC_MASK)) !=
	     (RX_FULL | (flags & CS_IDE) | (length << 16U))) ||
	    ((received_id & 0x1fffffffU) != id) ||
	    ((read_reg(tx) & CS_CODE_MASK) != TX_INACTIVE)) {
		sample_can_result.mismatches++;
		return -EBADMSG;
	}
	/* Bytes beyond DLC are unspecified and must not participate in the comparison. */
	for (uint32_t i = 0U; i < length; i++) {
		uint32_t shift = 24U - 8U * (i % 4U);

		if (((received_data[i / 4U] >> shift) & 0xffU) !=
		    ((data[i / 4U] >> shift) & 0xffU)) {
			sample_can_result.mismatches++;
			return -EBADMSG;
		}
	}
	if (extended) {
		sample_can_result.extended++;
	} else {
		sample_can_result.standard++;
	}
	sample_can_result.bytes += length;
	sample_can_result.dlc_seen |= BIT(length);
	sample_can_result.error_status |= read_reg(ESR1) & ERROR_MASK;
	sample_can_result.error_count = read_reg(ECR);
	sample_can_result.memory_errors |= read_reg(ERRSR);
	if ((sample_can_result.error_status != 0U) || (sample_can_result.error_count != 0U) ||
	    (sample_can_result.memory_errors != 0U)) {
		return -EIO;
	}
	return 0;
}

int32_t sample_can_loopback_test(void)
{
	int32_t err;

	if (!IS_ENABLED(CONFIG_SAMPLE_S32G_FLEXCAN_LOOPBACK)) {
		return 0;
	}
	sample_can_result = (struct can_loopback_result){.state = 1U, .stage = 1U};
	err = check_available();
	if (err == 0) {
		err = initialize();
		if (err == 0) {
			sample_can_result.stage = 6U;
			for (uint32_t i = 0U; i < FRAME_COUNT; i++) {
				err = transfer(i);
				if (err != 0) {
					break;
				}
			}
		}
		sample_can_result.error_status |= read_reg(ESR1) & ERROR_MASK;
		sample_can_result.error_count = read_reg(ECR);
		sample_can_result.memory_errors |= read_reg(ERRSR);
		/* Halt then disable CAN0, retaining LPB throughout cleanup and warm reload. */
		write_reg(MCR, read_reg(MCR) | MCR_FRZ | MCR_HALT);
		sample_can_result.cleanup_error = wait_reg(MCR, MCR_FRZACK, MCR_FRZACK);
		write_reg(MCR, read_reg(MCR) | MCR_MDIS);
		int32_t disable_error = wait_reg(MCR, MCR_LPMACK, MCR_LPMACK);

		if (sample_can_result.cleanup_error == 0) {
			sample_can_result.cleanup_error = disable_error;
		}
		if (err == 0) {
			err = sample_can_result.cleanup_error;
		}
		/* Include errors observed after the last frame and during module shutdown. */
		sample_can_result.error_status |= read_reg(ESR1) & ERROR_MASK;
		sample_can_result.memory_errors |= read_reg(ERRSR);
		uint32_t final_errors = read_reg(ECR);

		if (final_errors != 0U) {
			sample_can_result.error_count = final_errors;
		}
		if ((err == 0) && ((sample_can_result.error_status != 0U) ||
				   (sample_can_result.error_count != 0U) ||
				   (sample_can_result.memory_errors != 0U))) {
			err = -EIO;
		}
	}
	sample_can_result.final_mcr = read_reg(MCR);
	sample_can_result.error = err;
	sample_can_result.state = (err == 0) ? 2U : 3U;
	printk("CAN0 loopback %s: tx=%" PRIu32 " rx=%" PRIu32 " std=%" PRIu32
	       " ext=%" PRIu32 " bytes=%" PRIu32 " mismatch=%" PRIu32 " timeout=%" PRIu32 "\n",
	       (err == 0) ? "PASS" : "FAIL", sample_can_result.transmitted,
	       sample_can_result.received, sample_can_result.standard, sample_can_result.extended,
	       sample_can_result.bytes, sample_can_result.mismatches, sample_can_result.timeouts);
	printk("CAN0 loopback: clock=%" PRIu32 " bitrate=%" PRIu32 " err=%" PRId32
	       " stage=%" PRIu32 " esr=0x%08" PRIx32 " ecr=0x%08" PRIx32
	       " cleanup=%" PRId32 " disabled=%u\n",
	       sample_can_result.clock_hz, sample_can_result.bitrate, err, sample_can_result.stage,
	       sample_can_result.error_status, sample_can_result.error_count,
	       sample_can_result.cleanup_error,
	       (unsigned int)((sample_can_result.final_mcr & MCR_MDIS) != 0U));
	return err;
}
