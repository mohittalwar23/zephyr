/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_element.h>
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/mpipe_latency.h>

NET_BUF_POOL_FIXED_DEFINE(lat_pool, 1, 16, sizeof(struct mpipe_buffer_meta), NULL);

/* A source: source pad, no sink pad. A sink: the mirror. */
static struct mpipe_element source;
static struct mpipe_pad source_pad;
static struct mpipe_element sink;
static struct mpipe_pad sink_pad;

static void *setup(void)
{
	mpipe_element_init(&source, 1);
	mpipe_pad_init(&source_pad, 0, MPIPE_PAD_SRC, MPIPE_PAD_ALWAYS);
	mpipe_element_add_pad(&source, &source_pad);

	mpipe_element_init(&sink, 2);
	mpipe_pad_init(&sink_pad, 0, MPIPE_PAD_SINK, MPIPE_PAD_ALWAYS);
	mpipe_element_add_pad(&sink, &sink_pad);

	zassert_false(sys_dlist_is_empty(&source.src_pads), "source has no src pad");
	zassert_true(sys_dlist_is_empty(&source.sink_pads), "source has a sink pad");
	zassert_false(sys_dlist_is_empty(&sink.sink_pads), "sink has no sink pad");
	zassert_true(sys_dlist_is_empty(&sink.src_pads), "sink has a src pad");

	return NULL;
}

ZTEST(mpipe_latency, test_mark_measure_and_stats)
{
	struct mpipe_latency_stats stats;
	struct net_buf *buf;

	mpipe_latency_reset();

	/* Advance the clock so a source stamp is non-zero (0 = "never stamped"). */
	k_sleep(K_MSEC(5));

	buf = net_buf_alloc_len(&lat_pool, 16, K_NO_WAIT);
	zassert_not_null(buf);
	mpipe_buffer_get_meta(buf)->timestamp = 0U;

	mpipe_latency_mark(&sink, buf);
	zassert_equal(mpipe_buffer_get_meta(buf)->timestamp, 0U, "mark stamped a non-source");
	mpipe_latency_measure(&source, buf);
	mpipe_latency_get_stats(&stats);
	zassert_equal(stats.count, 0U, "measure counted a non-sink");

	mpipe_latency_mark(&source, buf);
	zassert_not_equal(mpipe_buffer_get_meta(buf)->timestamp, 0U, "source was not stamped");

	k_sleep(K_MSEC(20));

	mpipe_latency_measure(&sink, buf);
	mpipe_latency_get_stats(&stats);
	zassert_equal(stats.count, 1U, "sink measurement not counted");
	zassert_true(stats.last_us > 0U, "measured 0 us after a 20 ms delay");
	zassert_equal(stats.min_us, stats.last_us, "min != last for a single sample");
	zassert_equal(stats.max_us, stats.last_us, "max != last for a single sample");
	zassert_equal(stats.sum_us, (uint64_t)stats.last_us, "sum != last for a single sample");

	net_buf_unref(buf);
}

/* reset clears the accumulator, and min reads back as 0 (not the sentinel). */
ZTEST(mpipe_latency, test_reset)
{
	struct mpipe_latency_stats stats;

	mpipe_latency_reset();
	mpipe_latency_get_stats(&stats);

	zassert_equal(stats.count, 0U);
	zassert_equal(stats.min_us, 0U, "min should read 0 before any measurement");
	zassert_equal(stats.max_us, 0U);
	zassert_equal(stats.sum_us, 0U);
}

ZTEST_SUITE(mpipe_latency, NULL, setup, NULL, NULL, NULL);
