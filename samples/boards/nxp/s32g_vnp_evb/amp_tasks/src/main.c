/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "clock.h"
#include "led.h"

#define STACK_SIZE 1024U
#define CORE_CLOCK_HZ 400000000U
#define LED_CONFIG_ENABLED 0x4C454431U
#define LED_CONFIG_DISABLED 0x4E4C4544U

/* Each independent image exports core-specific debugger symbols. */
#if defined(CONFIG_SOC_S32G274A_M7_1)
#define CORE_ID 1U
#define TASK_1MS_CNT m7_1_task_1ms_cnt
#define TASK_5MS_CNT m7_1_task_5ms_cnt
#define TASK_10MS_CNT m7_1_task_10ms_cnt
#define TASK_20MS_CNT m7_1_task_20ms_cnt
#define TASK_1MS_MISSED m7_1_task_1ms_missed
#define TASK_5MS_MISSED m7_1_task_5ms_missed
#define TASK_10MS_MISSED m7_1_task_10ms_missed
#define TASK_20MS_MISSED m7_1_task_20ms_missed
#elif defined(CONFIG_SOC_S32G274A_M7_2)
#define CORE_ID 2U
#define TASK_1MS_CNT m7_2_task_1ms_cnt
#define TASK_5MS_CNT m7_2_task_5ms_cnt
#define TASK_10MS_CNT m7_2_task_10ms_cnt
#define TASK_20MS_CNT m7_2_task_20ms_cnt
#define TASK_1MS_MISSED m7_2_task_1ms_missed
#define TASK_5MS_MISSED m7_2_task_5ms_missed
#define TASK_10MS_MISSED m7_2_task_10ms_missed
#define TASK_20MS_MISSED m7_2_task_20ms_missed
#else
#error "The AMP application requires S32G274A M7_1 or M7_2"
#endif

atomic_t TASK_1MS_CNT;
atomic_t TASK_5MS_CNT;
atomic_t TASK_10MS_CNT;
atomic_t TASK_20MS_CNT;
atomic_t TASK_1MS_MISSED;
atomic_t TASK_5MS_MISSED;
atomic_t TASK_10MS_MISSED;
atomic_t TASK_20MS_MISSED;

uint32_t sample_amp_core_id;
uint32_t sample_amp_tasks_started;
int32_t sample_amp_error;

/* A file-backed marker lets the loader check GPIO ownership before reset release. */
const volatile uint32_t sample_amp_led_config =
	IS_ENABLED(CONFIG_SAMPLE_S32G_AMP_LED) ? LED_CONFIG_ENABLED : LED_CONFIG_DISABLED;

K_TIMER_DEFINE(timer_1ms, NULL, NULL);
K_TIMER_DEFINE(timer_5ms, NULL, NULL);
K_TIMER_DEFINE(timer_10ms, NULL, NULL);
K_TIMER_DEFINE(timer_20ms, NULL, NULL);

struct periodic_task {
	struct k_timer *timer;
	atomic_t *counter;
	atomic_t *missed;
};

static const struct periodic_task task_5ms = {
	.timer = &timer_5ms,
	.counter = &TASK_5MS_CNT,
	.missed = &TASK_5MS_MISSED,
};

static const struct periodic_task task_10ms = {
	.timer = &timer_10ms,
	.counter = &TASK_10MS_CNT,
	.missed = &TASK_10MS_MISSED,
};

static const struct periodic_task task_20ms = {
	.timer = &timer_20ms,
	.counter = &TASK_20MS_CNT,
	.missed = &TASK_20MS_MISSED,
};

static void record_periods(atomic_t *counter, atomic_t *missed, uint32_t elapsed)
{
	/* Timer status preserves expirations coalesced while a task was delayed. */
	atomic_add(counter, (atomic_val_t)elapsed);
	if (elapsed > 1U) {
		atomic_add(missed, (atomic_val_t)(elapsed - 1U));
	}
}

static void task_1ms(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t elapsed = k_timer_status_sync(&timer_1ms);

		record_periods(&TASK_1MS_CNT, &TASK_1MS_MISSED, elapsed);
		sample_amp_led_tick(elapsed);
	}
}

static void task_periodic(void *p1, void *p2, void *p3)
{
	const struct periodic_task *task = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t elapsed = k_timer_status_sync(task->timer);

		record_periods(task->counter, task->missed, elapsed);
	}
}

/* Static tasks stay stopped until the inherited clock and LED are ready. */
K_THREAD_DEFINE(task_1ms_id, STACK_SIZE, task_1ms, NULL, NULL, NULL, 0, 0, -1);
K_THREAD_DEFINE(task_5ms_id, STACK_SIZE, task_periodic, (void *)&task_5ms, NULL, NULL, 1, 0, -1);
K_THREAD_DEFINE(task_10ms_id, STACK_SIZE, task_periodic, (void *)&task_10ms, NULL, NULL, 2, 0, -1);
K_THREAD_DEFINE(task_20ms_id, STACK_SIZE, task_periodic, (void *)&task_20ms, NULL, NULL, 3, 0, -1);

int main(void)
{
	const k_tid_t threads[] = {
		task_1ms_id, task_5ms_id, task_10ms_id, task_20ms_id,
	};
	const char *const names[] = {
		"task_1ms", "task_5ms", "task_10ms", "task_20ms",
	};
	int32_t err;
	unsigned int key;

	sample_amp_core_id = CORE_ID;
	if (sample_clock_error != 0) {
		sample_amp_error = sample_clock_error;
		return sample_amp_error;
	}
	if (sample_core_clock_hz != CORE_CLOCK_HZ) {
		sample_amp_error = -ENOTSUP;
		return sample_amp_error;
	}
	if (sample_amp_led_config == LED_CONFIG_ENABLED) {
		err = sample_amp_led_initialize();
		if (err != 0) {
			sample_amp_error = err;
			return err;
		}
	} else if (sample_amp_led_config != LED_CONFIG_DISABLED) {
		sample_amp_error = -EINVAL;
		return sample_amp_error;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		err = k_thread_name_set(threads[i], names[i]);
		if (err != 0) {
			sample_amp_error = err;
			return err;
		}
	}

	/* Use one clock epoch; neither GPIO nor logging runs in a timer ISR. */
	key = irq_lock();
	k_timer_start(&timer_1ms, K_MSEC(1), K_MSEC(1));
	k_timer_start(&timer_5ms, K_MSEC(5), K_MSEC(5));
	k_timer_start(&timer_10ms, K_MSEC(10), K_MSEC(10));
	k_timer_start(&timer_20ms, K_MSEC(20), K_MSEC(20));
	irq_unlock(key);
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		k_thread_start(threads[i]);
	}
	sample_amp_tasks_started = 1U;
	return 0;
}
