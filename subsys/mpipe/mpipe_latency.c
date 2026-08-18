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

LOG_MODULE_REGISTER(mpipe_latency, CONFIG_MPIPE_LOG_LEVEL);

static struct mpipe_latency_stats stats = {.min_us = UINT32_MAX};
static struct mpipe_latency_bound e2e;
static struct k_spinlock lock;

void mpipe_latency_declare(uint32_t min_us, uint32_t max_us)
{
	K_SPINLOCK(&lock) {
		e2e.min_us += min_us;
		e2e.max_us += max_us;
	}
}

void mpipe_latency_get_e2e(struct mpipe_latency_bound *out)
{
	if (out == NULL) {
		return;
	}

	K_SPINLOCK(&lock) {
		*out = e2e;
	}
}

void mpipe_latency_mark(struct mpipe_element *elem, struct net_buf *buf)
{
	if (elem == NULL || buf == NULL) {
		return;
	}

	/* Only a source (no sink pads) stamps; a queue re-push keeps the original. */
	if (!sys_dlist_is_empty(&elem->sink_pads)) {
		return;
	}

	uint32_t now = k_cycle_get_32();

	/* 0 is the "never stamped" sentinel. */
	mpipe_buffer_get_meta(buf)->timestamp = (now == 0U) ? 1U : now;
}

void mpipe_latency_measure(struct mpipe_element *elem, struct net_buf *buf)
{
	uint32_t ingress;
	uint32_t us;

	if (elem == NULL || buf == NULL) {
		return;
	}

	if (!sys_dlist_is_empty(&elem->src_pads)) {
		return;
	}

	ingress = mpipe_buffer_get_meta(buf)->timestamp;
	if (ingress == 0U) {
		return;
	}

	us = k_cyc_to_us_near32(k_cycle_get_32() - ingress);

	K_SPINLOCK(&lock) {
		stats.count++;
		stats.last_us = us;
		stats.sum_us += us;
		stats.min_us = MIN(stats.min_us, us);
		stats.max_us = MAX(stats.max_us, us);
	}

	LOG_DBG("mpipe latency: sink=%u %u us", (unsigned int)elem->object.id, us);
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

void mpipe_latency_reset(void)
{
	K_SPINLOCK(&lock) {
		stats = (struct mpipe_latency_stats){.min_us = UINT32_MAX};
	}
}
