/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_bin.h>
#include <zephyr/mpipe/mpipe_element.h>
#include <zephyr/mpipe/mpipe_latency.h>
#include <zephyr/mpipe/mpipe_pipeline.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/aud/mpipe_aud.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_codec_sink.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_src.h>
#include <zephyr/mpipe/aud/mpipe_aud_src.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>

#define FRAME_INTERVAL_US 10000U
/* Long enough to exercise the running pipeline beyond the earlier 320 ms claim. */
#define RUN_MS 400

__nocache struct k_mem_slab test_slab;

static struct mpipe pipe;
static struct mpipe_aud_i2s_src src;
static struct mpipe_aud_i2s_codec_sink sink;
static struct mpipe_caps_filter caps_filter;

/*
 * The elements are static and linking them is a one-shot operation, so the
 * graph is built and run once for the whole suite and every test reads the
 * snapshot it produced. That also keeps the tests independent of the order
 * ztest happens to run them in.
 */
static struct mpipe_latency_stats g_transit;
static struct mpipe_latency_occupancy_stats g_occ;

static void run_pipeline(void)
{
	struct mpipe_structure caps;

	zassert_ok(mpipe_pipeline_init(&pipe, 0));
	zassert_ok(mpipe_aud_i2s_src_init(&src, 1));
	zassert_ok(mpipe_caps_filter_init(&caps_filter, 2));
	zassert_ok(mpipe_aud_i2s_codec_sink_init(&sink, 3));

	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&src,
					       MPIPE_PROP_AUD_SRC_SLAB_PTR, &test_slab,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&sink,
					       MPIPE_PROP_AUD_SINK_SLAB_PTR, &test_slab,
					       MPIPE_PROP_LIST_END));

	zassert_ok(mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					       MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT,
					       FRAME_INTERVAL_US, MPIPE_CAPS_NUM_OF_CHANNEL,
					       MPIPE_TYPE_UINT, 2, MPIPE_CAPS_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					       MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					       MPIPE_PROP_LIST_END));

	zassert_ok(mpipe_element_link((struct mpipe_element *)&src,
				      (struct mpipe_element *)&caps_filter,
				      (struct mpipe_element *)&sink, NULL));
	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&src,
				 (struct mpipe_element *)&caps_filter,
				 (struct mpipe_element *)&sink, NULL));

	mpipe_latency_reset();

	zassert_equal(mpipe_element_set_state((struct mpipe_element *)&pipe, MPIPE_STATE_PLAYING),
		      MPIPE_STATE_CHANGE_SUCCESS);
	k_msleep(RUN_MS);
	/*
	 * Keep the pool intact while stopping capture. READY teardown and its
	 * required source guard are covered by Zephyr pull request 114262.
	 */
	zassert_equal(mpipe_element_set_state((struct mpipe_element *)&pipe, MPIPE_STATE_PAUSED),
		      MPIPE_STATE_CHANGE_SUCCESS);

	mpipe_latency_get_stats(&g_transit);
	mpipe_latency_occupancy_get_stats(&g_occ);
}

static void *latency_suite_setup(void)
{
	run_pipeline();
	return NULL;
}

#if defined(CONFIG_MPIPE_LATENCY)

ZTEST(mpipe_aud_latency, test_transit_and_occupancy_are_measured)
{
	const struct mpipe_latency_stats transit = g_transit;
	const struct mpipe_latency_occupancy_stats occ = g_occ;

	zassert_true(transit.count > 0U, "no buffer reached the sink");
	zassert_true(transit.min_us <= transit.max_us, "transit min above max");
	zassert_true(transit.sum_us / transit.count <= transit.max_us, "mean above max");

	/* The audio pool answers get_occupancy, so occupancy must be sampled. */
	zassert_equal(occ.count, transit.count, "occupancy not sampled on every buffer");
	zassert_true(occ.capacity > 0U, "pool reported no capacity");
	zassert_true(occ.max_blocks <= occ.capacity, "more buffers in flight than the pool holds");
	zassert_true(occ.min_blocks <= occ.max_blocks, "occupancy min above max");
}

ZTEST(mpipe_aud_latency, test_measured_period_tracks_the_frame_interval)
{
	const struct mpipe_latency_occupancy_stats occ = g_occ;

	zassert_true(occ.count > 1U, "not enough buffers to measure an interval");

	/*
	 * native_sim does not pace this graph like physical audio hardware. This
	 * bound catches a missing or nonsensical interval; it does not prove that
	 * the value differs from the negotiated frame interval.
	 */
	zassert_true(occ.period_us < 10U * FRAME_INTERVAL_US,
		     "measured period %u us is implausible", occ.period_us);
}

ZTEST(mpipe_aud_latency, test_period_converts_after_accumulation)
{
	uint32_t one_us_cyc = k_us_to_cyc_floor32(1U);

	zassert_equal(mpipe_latency_mean_period_us((uint64_t)one_us_cyc * 3U, 3U), 1U,
		      "mean period conversion changed");
	zassert_equal(mpipe_latency_mean_period_us(100U, 0U), 0U,
		      "zero gaps must produce zero period");
}

ZTEST(mpipe_aud_latency, test_reset_clears_every_accumulator)
{
	struct mpipe_latency_stats transit;
	struct mpipe_latency_occupancy_stats occ;

	/* The suite already ran the graph, so there is something to clear. */
	mpipe_latency_reset();

	mpipe_latency_get_stats(&transit);
	mpipe_latency_occupancy_get_stats(&occ);

	zassert_equal(transit.count, 0U, "transit count survived a reset");
	zassert_equal(transit.min_us, 0U, "an empty accumulator must read 0, not UINT32_MAX");
	zassert_equal(occ.count, 0U, "occupancy count survived a reset");
	zassert_equal(occ.min_blocks, 0U, "an empty accumulator must read 0, not UINT32_MAX");
	zassert_equal(occ.period_us, 0U, "period survived a reset");
}

#else /* CONFIG_MPIPE_LATENCY */

ZTEST(mpipe_aud_latency, test_disabled_build_reports_nothing_and_still_runs)
{
	const struct mpipe_latency_stats transit = g_transit;
	const struct mpipe_latency_occupancy_stats occ = g_occ;

	/* The instrumented call sites compiled out, and the graph still ran. */
	zassert_equal(transit.count, 0U, "a disabled build reported transit");
	zassert_equal(occ.count, 0U, "a disabled build reported occupancy");
}

#endif /* CONFIG_MPIPE_LATENCY */

ZTEST_SUITE(mpipe_aud_latency, NULL, latency_suite_setup, NULL, NULL, NULL);
