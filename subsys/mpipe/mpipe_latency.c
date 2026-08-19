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
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_latency.h>

LOG_MODULE_REGISTER(mpipe_latency, CONFIG_MPIPE_LOG_LEVEL);

static struct mpipe_latency_stats stats = {.min_us = UINT32_MAX};
static struct k_spinlock lock;

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

void mpipe_latency_set_report(struct mpipe_element *elem, mpipe_latency_report_fn fn)
{
	if (elem != NULL) {
		elem->report_latency = fn;
	}
}

static struct mpipe_pad *first_pad(sys_dlist_t *pads)
{
	struct mpipe_object *pad_obj = NULL;

	pad_obj = SYS_DLIST_PEEK_HEAD_CONTAINER(pads, pad_obj, node);
	return (struct mpipe_pad *)pad_obj;
}

void mpipe_latency_query(struct mpipe_element *elem, struct mpipe_latency_bound *out)
{
	*out = (struct mpipe_latency_bound){0};
	if (elem == NULL) {
		return;
	}

	/* Walk up to the source (element with no sink pads). */
	struct mpipe_element *src = elem;
	struct mpipe_pad *sink_pad;

	while ((sink_pad = first_pad(&src->sink_pads)) != NULL && sink_pad->peer != NULL) {
		src = (struct mpipe_element *)sink_pad->peer->object.container;
	}

	/* Kick the LATENCY query off on the source's src pad; it sums downstream. */
	struct mpipe_pad *src_pad = first_pad(&src->src_pads);

	if (src_pad == NULL) {
		return;
	}

	struct mpipe_dispatch q = {.type = MPIPE_DISPATCH_LATENCY};

	(void)mpipe_pad_query(src_pad, &q);
	*out = q.latency;
}
