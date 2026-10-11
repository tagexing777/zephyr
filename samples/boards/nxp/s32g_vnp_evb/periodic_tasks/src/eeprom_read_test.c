/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "clock.h"
#include "eeprom_read_test.h"

/* S32G I2C has byte-spaced registers and active-high module disable. */
#define I2C2_BASE       0x401ec000U
#define IBFD            1U
#define IBCR            2U
#define IBSR            3U
#define IBDR            4U
#define IBIC            5U
#define IBDBG           6U
#define MDIS            BIT(7)
#define MSSL            BIT(5)
#define TXRX            BIT(4)
#define NOACK           BIT(3)
#define RSTA            BIT(2)
#define TCF             BIT(7)
#define IBB             BIT(5)
#define IBAL            BIT(4)
#define IBIF            BIT(1)
#define RXAK            BIT(0)
#define GLFLT_EN        BIT(3)
#define EEPROM_ADDRESS  0x51U
#define EEPROM_SIZE     SAMPLE_EEPROM_SIZE
#define READ_ROUNDS     100U
#define TIMEOUT_US      20000U

PINCTRL_DT_DEFINE(DT_PATH(zephyr_user));

/* Debugger-visible evidence: state 0/not run, 1/running, 2/pass, 3/fail. */
struct eeprom_read_result {
	uint32_t state;
	int32_t error;
	uint32_t transactions;
	uint32_t bytes;
	uint32_t block_matches;
	uint32_t offset_matches;
	uint32_t nacks;
	uint32_t timeouts;
	uint32_t arbitration_losses;
	uint32_t bus_hz;
	uint32_t last_status;
	uint32_t checksum;
	uint8_t baseline[EEPROM_SIZE];
};

volatile struct eeprom_read_result sample_eeprom_result;

/* Zero after each ELF startup. Only set after saving the complete host backup. */
volatile uint32_t sample_eeprom_write_arm;

struct eeprom_write_result {
	uint32_t state;
	int32_t error;
	int32_t restore_error;
	uint32_t page_matches;
	uint32_t byte_matches;
	uint32_t write_cycles;
	uint32_t restore_attempts;
	uint32_t restored;
	uint32_t final_hash;
	uint32_t write_bytes;
	uint8_t baseline[EEPROM_SIZE];
	uint8_t final[EEPROM_SIZE];
};

volatile struct eeprom_write_result sample_eeprom_write_result;

static uint8_t reg_read(uint32_t offset)
{
	return sys_read8(I2C2_BASE + offset);
}

static void reg_write(uint32_t offset, uint8_t value)
{
	sys_write8(value, I2C2_BASE + offset);
}

static int32_t wait_status(uint8_t mask, bool set)
{
	uint32_t start = k_cycle_get_32();
	uint32_t bound = k_us_to_cyc_ceil32(TIMEOUT_US);

	do {
		uint8_t status = reg_read(IBSR);

		sample_eeprom_result.last_status = status;
		if ((status & IBAL) != 0U) {
			sample_eeprom_result.arbitration_losses++;
			return -EAGAIN;
		}
		if (((status & mask) != 0U) == set) {
			return 0;
		}
	} while ((uint32_t)(k_cycle_get_32() - start) < bound);
	sample_eeprom_result.timeouts++;
	return -ETIMEDOUT;
}

static int32_t wait_byte(bool transmit)
{
	int32_t err = wait_status(IBIF, true);
	uint8_t status = reg_read(IBSR);

	if (err != 0) {
		return err;
	}
	/* IBIF and IBAL are write-one-to-clear, not read-modify-write. */
	reg_write(IBSR, IBIF);
	if ((status & TCF) == 0U) {
		return -EIO;
	}
	if (transmit && ((status & RXAK) != 0U)) {
		sample_eeprom_result.nacks++;
		return -ENXIO;
	}
	return 0;
}

static int32_t transmit_byte(uint8_t value)
{
	reg_write(IBDR, value);
	return wait_byte(true);
}

static int32_t stop_transfer(int32_t err)
{
	/* Do not drive STOP after arbitration loss: another master owns the bus. */
	if (err != -EAGAIN) {
		int32_t stop_err;

		reg_write(IBCR, TXRX);
		stop_err = wait_status(IBB, false);
		if (err == 0) {
			err = stop_err;
		}
	}
	reg_write(IBSR, IBIF | IBAL);
	return err;
}

static int32_t read_offset(uint8_t offset, uint8_t *buffer, size_t length)
{
	int32_t err;

	if ((buffer == NULL) || (length == 0U) ||
	    ((size_t)offset + length > EEPROM_SIZE)) {
		return -EINVAL;
	}
	err = wait_status(IBB, false);
	if (err != 0) {
		return err;
	}
	reg_write(IBSR, IBIF | IBAL);
	reg_write(IBCR, MSSL | TXRX);
	err = wait_status(IBB, true);
	if (err == 0) {
		err = transmit_byte(EEPROM_ADDRESS << 1U);
	}
	if (err == 0) {
		/* Only the word address is sent: no EEPROM data programming. */
		err = transmit_byte(offset);
	}
	if (err == 0) {
		reg_write(IBCR, MSSL | TXRX | RSTA);
		err = transmit_byte((EEPROM_ADDRESS << 1U) | 1U);
	}
	if (err != 0) {
		goto stop;
	}
	reg_write(IBCR, MSSL | ((length == 1U) ? NOACK : 0U));
	(void)reg_read(IBDR); /* Dummy read starts the receive shifter. */
	for (size_t i = 0U; i < length; i++) {
		err = wait_byte(false);
		if (err != 0) {
			goto stop;
		}
		if (i + 1U == length) {
			/* STOP must precede reading the final byte. */
			reg_write(IBCR, NOACK);
		} else if (i + 2U == length) {
			reg_write(IBCR, MSSL | NOACK);
		}
		buffer[i] = reg_read(IBDR);
	}
stop:
	err = stop_transfer(err);
	if (err == 0) {
		sample_eeprom_result.transactions++;
		sample_eeprom_result.bytes += length;
	}
	return err;
}

static uint32_t buffer_hash(const uint8_t *buffer, size_t length)
{
	uint32_t hash = 2166136261U;

	for (size_t i = 0U; i < length; i++) {
		hash ^= buffer[i];
		hash *= 16777619U;
	}
	return hash;
}

static int32_t write_page(uint8_t offset, const uint8_t *buffer, size_t length)
{
	int32_t err;

	if ((buffer == NULL) || !sample_eeprom_write_range_valid(offset, length)) {
		return -EINVAL;
	}
	err = wait_status(IBB, false);
	if (err != 0) {
		return err;
	}
	reg_write(IBSR, IBIF | IBAL);
	reg_write(IBCR, MSSL | TXRX);
	err = wait_status(IBB, true);
	if (err == 0) {
		err = transmit_byte(EEPROM_ADDRESS << 1U);
	}
	if (err == 0) {
		err = transmit_byte(offset);
	}
	for (size_t i = 0U; (i < length) && (err == 0); i++) {
		err = transmit_byte(buffer[i]);
	}
	err = stop_transfer(err);
	/* AT24C01B/C tWR <= 5 ms. Also wait after a possibly partial write. */
	k_msleep(6);
	if (err == 0) {
		sample_eeprom_write_result.write_cycles++;
		sample_eeprom_write_result.write_bytes += length;
	}
	return err;
}

static int32_t write_compare(uint8_t offset, const uint8_t *buffer, size_t length)
{
	uint8_t readback[SAMPLE_EEPROM_PAGE_SIZE];
	int32_t err = write_page(offset, buffer, length);

	if (err == 0) {
		err = read_offset(offset, readback, length);
	}
	if ((err == 0) && (memcmp(buffer, readback, length) != 0)) {
		err = -EILSEQ;
	}
	return err;
}

int32_t sample_eeprom_write_test(void)
{
	const uint8_t patterns[] = {0x00U, 0xffU, 0xaaU, 0x55U};
	uint8_t baseline[EEPROM_SIZE];
	uint8_t final[EEPROM_SIZE];
	uint8_t pattern[SAMPLE_EEPROM_PAGE_SIZE];
	int32_t err = 0;
	int32_t restore_err = 0;

	if (sample_eeprom_write_arm != SAMPLE_EEPROM_WRITE_ARM) {
		return 0;
	}
	sample_eeprom_write_arm = 0U;
	sample_eeprom_write_result.state = 1U;
	if ((sample_eeprom_result.state != 2U) || ((reg_read(IBCR) & MDIS) == 0U)) {
		err = -EPERM;
		goto finish;
	}
	for (size_t i = 0U; i < sizeof(baseline); i++) {
		baseline[i] = sample_eeprom_result.baseline[i];
		sample_eeprom_write_result.baseline[i] = baseline[i];
	}
	reg_write(IBSR, IBIF | IBAL);
	reg_write(IBCR, TXRX);
	err = read_offset(0U, final, sizeof(final));
	if (err == 0) {
		if (memcmp(baseline, final, sizeof(final)) != 0) {
			err = -EILSEQ;
		}
	}
	if (err != 0) {
		goto disable;
	}
	printk("I2C2 U65 RW START range=0x78-0x7f backup=%08" PRIx32 "\n",
	       buffer_hash(baseline, sizeof(baseline)));
	for (size_t i = 0U; i < ARRAY_SIZE(patterns); i++) {
		memset(pattern, patterns[i], sizeof(pattern));
		err = write_compare(SAMPLE_EEPROM_TEST_OFFSET, pattern, sizeof(pattern));
		if (err != 0) {
			goto restore;
		}
		sample_eeprom_write_result.page_matches++;
	}
	for (size_t i = 0U; i < sizeof(pattern); i++) {
		pattern[i] = 0x3cU ^ (uint8_t)i;
	}
	err = write_compare(SAMPLE_EEPROM_TEST_OFFSET, pattern, sizeof(pattern));
	if (err != 0) {
		goto restore;
	}
	sample_eeprom_write_result.page_matches++;
	pattern[0] = 0x69U;
	err = write_compare(SAMPLE_EEPROM_TEST_OFFSET, pattern, 1U);
	if (err != 0) {
		goto restore;
	}
	sample_eeprom_write_result.byte_matches++;
	pattern[0] = 0x96U;
	err = write_compare(EEPROM_SIZE - 1U, pattern, 1U);
	if (err == 0) {
		sample_eeprom_write_result.byte_matches++;
	}
restore:
	/* Even a failed transaction may have programmed data before its STOP. */
	for (uint32_t attempt = 0U; attempt < 3U; attempt++) {
		sample_eeprom_write_result.restore_attempts++;
		restore_err = write_page(SAMPLE_EEPROM_TEST_OFFSET,
					 baseline + SAMPLE_EEPROM_TEST_OFFSET, sizeof(pattern));
		if (restore_err == 0) {
			restore_err = read_offset(0U, final, sizeof(final));
		}
		if (restore_err == 0) {
			for (size_t i = 0U; i < sizeof(final); i++) {
				sample_eeprom_write_result.final[i] = final[i];
			}
			sample_eeprom_write_result.final_hash = buffer_hash(final, sizeof(final));
			if (memcmp(baseline, final, sizeof(final)) == 0) {
				sample_eeprom_write_result.restored = 1U;
				break;
			}
			restore_err = -EILSEQ;
		}
		k_msleep(6);
	}
	if ((err == 0) && (restore_err != 0)) {
		err = restore_err;
	}
disable:
	reg_write(IBCR, MDIS);
finish:
	sample_eeprom_write_result.error = err;
	sample_eeprom_write_result.restore_error = restore_err;
	sample_eeprom_write_result.state =
		((err == 0) && (sample_eeprom_write_result.restored == 1U)) ? 2U : 3U;
	printk("I2C2 U65 RW %s err=%" PRId32 " pages=%" PRIu32 " bytes=%" PRIu32
	       " cycles=%" PRIu32 " restore=%" PRIu32 " restore_err=%" PRId32
	       " hash=%08" PRIx32 "\n",
	       (sample_eeprom_write_result.state == 2U) ? "PASS" : "FAIL", err,
	       sample_eeprom_write_result.page_matches, sample_eeprom_write_result.byte_matches,
	       sample_eeprom_write_result.write_cycles, sample_eeprom_write_result.restored,
	       restore_err, sample_eeprom_write_result.final_hash);
	return err;
}

void sample_eeprom_read_test(void)
{
	const uint8_t offsets[] = {0U, 1U, 7U, 8U, 63U, 127U};
	const struct pinctrl_dev_config *pins = PINCTRL_DT_DEV_CONFIG_GET(DT_PATH(zephyr_user));
	uint8_t baseline[EEPROM_SIZE];
	uint8_t buffer[EEPROM_SIZE];
	int32_t err;

	sample_eeprom_result.state = 1U;
	/* Fixed bring-up divider assumes the existing 400 MHz M7 / XBAR clock. */
	if (sample_core_clock_hz != 400000000U) {
		err = -ENOTSUP;
		goto finish;
	}
	if ((reg_read(IBCR) & MDIS) == 0U) {
		err = -EBUSY;
		goto finish;
	}
	err = pinctrl_apply_state(pins, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		goto finish;
	}
	/* MUL=4, prescaler=5, tap=1: divider 4*2*(30+5*32+2)=1536. */
	reg_write(IBFD, 0xa9U);
	reg_write(IBIC, 0U);
	reg_write(IBDBG, GLFLT_EN);
	reg_write(IBSR, IBIF | IBAL);
	reg_write(IBCR, TXRX);
	sample_eeprom_result.bus_hz = sample_core_clock_hz / 3U / 1536U;
	printk("I2C2 U65 read-only addr=0x51 bus=%" PRIu32 " Hz\n",
	       sample_eeprom_result.bus_hz);
	err = read_offset(0U, baseline, sizeof(baseline));
	if (err != 0) {
		goto disable;
	}
	for (size_t i = 0U; i < sizeof(baseline); i++) {
		sample_eeprom_result.baseline[i] = baseline[i];
	}
	for (uint32_t round = 0U; round < READ_ROUNDS; round++) {
		err = read_offset(0U, buffer, sizeof(buffer));
		if (err != 0) {
			goto disable;
		}
		if (memcmp(baseline, buffer, sizeof(buffer)) != 0) {
			err = -EILSEQ;
			goto disable;
		}
		sample_eeprom_result.block_matches++;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(offsets); i++) {
		err = read_offset(offsets[i], buffer, 1U);
		if (err != 0) {
			goto disable;
		}
		if (buffer[0] != baseline[offsets[i]]) {
			err = -EILSEQ;
			goto disable;
		}
		sample_eeprom_result.offset_matches++;
	}
	sample_eeprom_result.checksum = buffer_hash(baseline, sizeof(baseline));
	for (size_t i = 0U; i < sizeof(baseline); i += 16U) {
		printk("I2C2 U65 %02x:", (unsigned int)i);
		for (size_t j = 0U; j < 16U; j++) {
			printk(" %02x", baseline[i + j]);
		}
		printk("\n");
	}
disable:
	reg_write(IBCR, MDIS);
finish:
	sample_eeprom_result.error = err;
	sample_eeprom_result.state = (err == 0) ? 2U : 3U;
	printk("I2C2 U65 %s err=%" PRId32 " blocks=%" PRIu32 " offsets=%" PRIu32
	       " transfers=%" PRIu32 " bytes=%" PRIu32 " nack=%" PRIu32
	       " timeout=%" PRIu32 " arb=%" PRIu32 " hash=%08" PRIx32 "\n",
	       (err == 0) ? "PASS" : "FAIL", err, sample_eeprom_result.block_matches,
	       sample_eeprom_result.offset_matches, sample_eeprom_result.transactions,
	       sample_eeprom_result.bytes, sample_eeprom_result.nacks,
	       sample_eeprom_result.timeouts, sample_eeprom_result.arbitration_losses,
	       sample_eeprom_result.checksum);
}
