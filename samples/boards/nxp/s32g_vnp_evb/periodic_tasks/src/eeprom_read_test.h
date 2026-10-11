/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S32G_PERIODIC_TASKS_EEPROM_READ_TEST_H_
#define S32G_PERIODIC_TASKS_EEPROM_READ_TEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_EEPROM_SIZE         128U
#define SAMPLE_EEPROM_PAGE_SIZE    8U
#define SAMPLE_EEPROM_TEST_OFFSET  0x78U
#define SAMPLE_EEPROM_WRITE_ARM    0x45575254U

/* Read upper-board U65 without programming EEPROM data. Reports over UART. */
void sample_eeprom_read_test(void);

/* Optional debugger-armed test; return zero only when skipped or fully restored. */
int32_t sample_eeprom_write_test(void);

/* Only the last page can be written, without wrapping the EEPROM page counter. */
static inline bool sample_eeprom_write_range_valid(uint8_t offset, size_t length)
{
	return (offset >= SAMPLE_EEPROM_TEST_OFFSET) && (offset < SAMPLE_EEPROM_SIZE) &&
	       (length > 0U) && (length <= SAMPLE_EEPROM_SIZE - offset) &&
	       (length <= SAMPLE_EEPROM_PAGE_SIZE - (offset % SAMPLE_EEPROM_PAGE_SIZE));
}

#endif /* S32G_PERIODIC_TASKS_EEPROM_READ_TEST_H_ */
