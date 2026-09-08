/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/dlist.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_element.h>
#include <zephyr/mpipe/mpipe_latency.h>
#include <zephyr/mpipe/mpipe_pad.h>

LOG_MODULE_REGISTER(mpipe_latency, CONFIG_MPIPE_LOG_LEVEL);

/*
 * One lock covers every accumulator. A linear pipeline runs its elements on
 * one thread and never contends; a tee or a queue element adds a second
 * writer, which is the case the lock exists for. None of this file is
 * compiled with CONFIG_MPIPE_LATENCY off.
 */
static struct k_spinlock lock;

static struct mpipe_latency_stats stats = {.min_us = UINT32_MAX};
static struct mpipe_latency_occupancy_stats occupancy = {.min_blocks = UINT32_MAX};

/*
 * Accumulate the gap between consecutive buffers rather than spanning the
 * first to the last: the cycle counter is 32-bit and wraps after only 5.4 s at
 * 800 MHz, which any useful measurement window exceeds. One inter-buffer gap
 * never comes close to it.
 */
static uint32_t prev_cyc;
static uint64_t interval_cyc;

void mpipe_latency_mark(struct mpipe_element *elem, struct net_buf *buf)
{
	struct mpipe_buffer_meta *meta;
	uint32_t now;

	if (elem == NULL || buf == NULL) {
		return;
	}

	/* Only a source stamps; a re-push keeps the original stamp. */
	if (!sys_dlist_is_empty(&elem->sink_pads)) {
		return;
	}

	now = k_cycle_get_32();
	meta = mpipe_buffer_get_meta(buf);

	/* 0 is the "never stamped" sentinel. */
	meta->latency_ingress_cyc = (now == 0U) ? 1U : now;
}

void mpipe_latency_measure(struct mpipe_element *elem, struct net_buf *buf)
{
	struct mpipe_buffer_meta *meta;
	uint32_t ingress;
	uint32_t now;
	uint32_t us;
	uint32_t in_flight = 0U;
	uint32_t capacity = 0U;
	bool have_occupancy = false;

	if (elem == NULL || buf == NULL) {
		return;
	}

	/* Only a sink measures. */
	if (!sys_dlist_is_empty(&elem->src_pads)) {
		return;
	}

	meta = mpipe_buffer_get_meta(buf);
	ingress = meta->latency_ingress_cyc;
	if (ingress == 0U) {
		return;
	}

	now = k_cycle_get_32();
	us = mpipe_latency_cyc_delta_to_us(ingress, now);

	/*
	 * Ask the pool, not the element, so this stays plugin-agnostic: audio,
	 * video or anything else that implements the optional hook is measured
	 * the same way, and a pool that cannot answer is simply not sampled.
	 */
	if (meta->pool != NULL &&
	    mpipe_buffer_pool_get_occupancy(meta->pool, &in_flight, &capacity) == 0) {
		have_occupancy = true;
	}

	K_SPINLOCK(&lock) {
		stats.count++;
		stats.sum_us += us;
		stats.min_us = MIN(stats.min_us, us);
		stats.max_us = MAX(stats.max_us, us);

		if (have_occupancy) {
			if (occupancy.count > 0U) {
				interval_cyc += (uint32_t)(now - prev_cyc);
			}
			prev_cyc = now;

			occupancy.count++;
			occupancy.capacity = capacity;
			occupancy.sum_blocks += in_flight;
			occupancy.min_blocks = MIN(occupancy.min_blocks, in_flight);
			occupancy.max_blocks = MAX(occupancy.max_blocks, in_flight);
		}
	}
}

void mpipe_latency_get_stats(struct mpipe_latency_stats *out)
{
	if (out == NULL) {
		return;
	}

	K_SPINLOCK(&lock) {
		*out = stats;
	}

	if (out->count == 0U) {
		out->min_us = 0U;
	}
}

void mpipe_latency_occupancy_get_stats(struct mpipe_latency_occupancy_stats *out)
{
	if (out == NULL) {
		return;
	}

	K_SPINLOCK(&lock) {
		*out = occupancy;

		/* Sampling is once per buffer, so the span covers count - 1 gaps. */
		if (occupancy.count > 1U) {
			out->period_us =
				mpipe_latency_mean_period_us(interval_cyc, occupancy.count - 1U);
		}
	}

	if (out->count == 0U) {
		out->min_blocks = 0U;
	}
}

void mpipe_latency_reset(void)
{
	K_SPINLOCK(&lock) {
		stats = (struct mpipe_latency_stats){.min_us = UINT32_MAX};
		occupancy = (struct mpipe_latency_occupancy_stats){.min_blocks = UINT32_MAX};
		prev_cyc = 0U;
		interval_cyc = 0U;
	}
}
