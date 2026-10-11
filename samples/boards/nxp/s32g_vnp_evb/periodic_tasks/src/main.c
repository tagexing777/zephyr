/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "clock.h"
#include "periodic_logic.h"

#define STACK_SIZE   1024U
#define EVENT_5MS    BIT(0)
#define EVENT_10MS   BIT(1)
#define EVENT_20MS   BIT(2)
#define EVENT_ALL    (EVENT_5MS | EVENT_10MS | EVENT_20MS)

/* Atomic fields remain visible in TRACE32 and safe to read across threads. */
struct sample_counters {
	atomic_t task_1ms;
	atomic_t task_10ms;
	atomic_t task_100ms;
	atomic_t event_5ms;
	atomic_t event_10ms;
	atomic_t event_20ms;
	atomic_t missed_1ms;
	atomic_t missed_10ms;
	atomic_t missed_100ms;
	atomic_t led_toggles;
	atomic_t gpio_errors;
};

struct sample_counters sample_counters;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static K_EVENT_DEFINE(extended_events);
static K_MUTEX_DEFINE(console_lock);

static atomic_t pending_5ms;
static atomic_t pending_10ms;
static atomic_t pending_20ms;

static void event_expiry(struct k_timer *timer);

K_TIMER_DEFINE(timer_1ms, NULL, NULL);
K_TIMER_DEFINE(timer_10ms, NULL, NULL);
K_TIMER_DEFINE(timer_100ms, NULL, NULL);
K_TIMER_DEFINE(timer_event_5ms, event_expiry, NULL);
K_TIMER_DEFINE(timer_event_10ms, event_expiry, NULL);
K_TIMER_DEFINE(timer_event_20ms, event_expiry, NULL);

static void event_expiry(struct k_timer *timer)
{
	atomic_t *pending;
	uint32_t event;

	if (timer == &timer_event_5ms) {
		pending = &pending_5ms;
		event = EVENT_5MS;
	} else if (timer == &timer_event_10ms) {
		pending = &pending_10ms;
		event = EVENT_10MS;
	} else {
		pending = &pending_20ms;
		event = EVENT_20MS;
	}

	/* Event bits coalesce; the pending count preserves every expiration. */
	atomic_inc(pending);
	k_event_post(&extended_events, event);
}

static void record_periods(atomic_t *counter, atomic_t *missed, uint32_t elapsed)
{
	atomic_add(counter, (atomic_val_t)elapsed);
	if (elapsed > 1U) {
		atomic_add(missed, (atomic_val_t)(elapsed - 1U));
	}
}

static void task_1ms(void *p1, void *p2, void *p3)
{
	uint32_t led_phase = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t elapsed = k_timer_status_sync(&timer_1ms);

		record_periods(&sample_counters.task_1ms, &sample_counters.missed_1ms, elapsed);
		if (sample_led_advance(&led_phase, elapsed)) {
			int err = gpio_pin_toggle_dt(&led);

			if (err != 0) {
				atomic_inc(&sample_counters.gpio_errors);
				return;
			}
			atomic_inc(&sample_counters.led_toggles);
		}
	}
}

struct periodic_log {
	struct k_timer *timer;
	atomic_t *counter;
	atomic_t *missed;
	const char *name;
};

static const struct periodic_log log_10ms = {
	.timer = &timer_10ms,
	.counter = &sample_counters.task_10ms,
	.missed = &sample_counters.missed_10ms,
	.name = "task_10ms",
};

static const struct periodic_log log_100ms = {
	.timer = &timer_100ms,
	.counter = &sample_counters.task_100ms,
	.missed = &sample_counters.missed_100ms,
	.name = "task_100ms",
};

static void task_log(void *p1, void *p2, void *p3)
{
	const struct periodic_log *task = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t elapsed = k_timer_status_sync(task->timer);
		int err;

		record_periods(task->counter, task->missed, elapsed);
		err = k_mutex_lock(&console_lock, K_FOREVER);
		if (err != 0) {
			return;
		}
		if (task == &log_100ms) {
			printk("%s count=%" PRIu32 " e5=%" PRIu32 " e10=%" PRIu32
			       " e20=%" PRIu32 "\n", task->name,
			       (uint32_t)atomic_get(task->counter),
			       (uint32_t)atomic_get(&sample_counters.event_5ms),
			       (uint32_t)atomic_get(&sample_counters.event_10ms),
			       (uint32_t)atomic_get(&sample_counters.event_20ms));
		} else {
			printk("%s count=%" PRIu32 "\n", task->name,
			       (uint32_t)atomic_get(task->counter));
		}
		err = k_mutex_unlock(&console_lock);
		if (err != 0) {
			return;
		}
	}
}

static void task_extended(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t events = k_event_wait_safe(&extended_events, EVENT_ALL, false, K_FOREVER);

		if ((events & EVENT_5MS) != 0U) {
			atomic_add(&sample_counters.event_5ms, atomic_set(&pending_5ms, 0));
		}
		if ((events & EVENT_10MS) != 0U) {
			atomic_add(&sample_counters.event_10ms, atomic_set(&pending_10ms, 0));
		}
		if ((events & EVENT_20MS) != 0U) {
			atomic_add(&sample_counters.event_20ms, atomic_set(&pending_20ms, 0));
		}
	}
}

/* A delay of -1 keeps static threads stopped until main() configures GPIO. */
K_THREAD_DEFINE(task_1ms_id, STACK_SIZE, task_1ms, NULL, NULL, NULL, 0, 0, -1);
K_THREAD_DEFINE(task_extended_id, STACK_SIZE, task_extended, NULL, NULL, NULL, 1, 0, -1);
K_THREAD_DEFINE(task_10ms_id, STACK_SIZE, task_log, (void *)&log_10ms, NULL, NULL, 2, 0, -1);
K_THREAD_DEFINE(task_100ms_id, STACK_SIZE, task_log, (void *)&log_100ms, NULL, NULL, 3, 0, -1);

int main(void)
{
	const k_tid_t threads[] = {
		task_1ms_id, task_extended_id, task_10ms_id, task_100ms_id,
	};
	const char *const names[] = {
		"task_1ms", "task_extended", "task_10ms", "task_100ms",
	};
	int err;

	if (sample_core_clock_hz == 0U) {
		printk("Unsupported M7 clock tree, MUX0 source=%" PRIu32 "\n", sample_clock_source);
		return -ENOTSUP;
	}
	if (!gpio_is_ready_dt(&led)) {
		printk("D15 GPIO is not ready\n");
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		printk("D15 GPIO configuration failed: %d\n", err);
		return err;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		err = k_thread_name_set(threads[i], names[i]);
		if (err != 0) {
			printk("Thread naming failed: %d\n", err);
			return err;
		}
	}

	printk("M7 clock=%" PRIu32 " Hz, MUX0 source=%" PRIu32 ", tick=1 ms\n",
	       sample_core_clock_hz, sample_clock_source);
	printk("Periodic tasks started; D15/PB8 toggles every 500 ms\n");

	/* Start all deadlines together; no UART or GPIO operations run in the ISR. */
	unsigned int key = irq_lock();

	k_timer_start(&timer_1ms, K_MSEC(1), K_MSEC(1));
	k_timer_start(&timer_10ms, K_MSEC(10), K_MSEC(10));
	k_timer_start(&timer_100ms, K_MSEC(100), K_MSEC(100));
	k_timer_start(&timer_event_5ms, K_MSEC(5), K_MSEC(5));
	k_timer_start(&timer_event_10ms, K_MSEC(10), K_MSEC(10));
	k_timer_start(&timer_event_20ms, K_MSEC(20), K_MSEC(20));
	irq_unlock(key);
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		k_thread_start(threads[i]);
	}
	return 0;
}
