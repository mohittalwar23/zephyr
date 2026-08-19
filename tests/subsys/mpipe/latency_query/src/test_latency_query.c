/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_latency.h>
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_value.h>
#include <zephyr/mpipe/aud/mpipe_aud.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_src.h>
#include <zephyr/mpipe/aud/mpipe_aud_gain.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_codec_sink.h>

static struct mpipe pipe;
static struct mpipe_aud_i2s_src source;
static struct mpipe_aud_gain gain;
static struct mpipe_aud_i2s_codec_sink sink;

static void stub_20_50(struct mpipe_element *e, struct mpipe_latency_bound *o)
{
	ARG_UNUSED(e);
	o->min_us = 20U;
	o->max_us = 50U;
}

static void stub_5_5(struct mpipe_element *e, struct mpipe_latency_bound *o)
{
	ARG_UNUSED(e);
	o->min_us = 5U;
	o->max_us = 5U;
}

ZTEST(mpipe_latency_query, test_query_sums_source_and_sink)
{
	struct mpipe_element *src_e = (struct mpipe_element *)&source;
	struct mpipe_element *gain_e = (struct mpipe_element *)&gain;
	struct mpipe_element *sink_e = (struct mpipe_element *)&sink;

	zassert_ok(mpipe_pipeline_init(&pipe, 0));
	zassert_ok(mpipe_aud_i2s_src_init(&source, 1));
	zassert_ok(mpipe_aud_gain_init(&gain, 2));
	zassert_ok(mpipe_aud_i2s_codec_sink_init(&sink, 3));
	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipe, src_e, gain_e, sink_e, NULL));
	zassert_ok(mpipe_element_link(src_e, gain_e, sink_e, NULL));

	mpipe_latency_set_report(src_e, stub_20_50);
	mpipe_latency_set_report(sink_e, stub_5_5);

	struct mpipe_latency_bound b;

	mpipe_latency_query(gain_e, &b);
	zassert_equal(b.min_us, 25U, "min: got %u", b.min_us);
	zassert_equal(b.max_us, 55U, "max: got %u", b.max_us);
}

ZTEST(mpipe_latency_query, test_source_reports_one_frame)
{
	struct mpipe_aud_i2s_src s;

	zassert_ok(mpipe_aud_i2s_src_init(&s, 1));

	struct mpipe_structure caps;

	zassert_ok(mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					       MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT, 10000,
					       MPIPE_CAPS_END));
	mpipe_pad_set_caps(&s.aud_src.src.src_pad, &caps);

	struct mpipe_latency_bound b = {0};

	s.aud_src.src.element.report_latency((struct mpipe_element *)&s, &b);
	zassert_equal(b.min_us, 10000U, "min: got %u", b.min_us);
	zassert_equal(b.max_us, 10000U, "max: got %u", b.max_us);
}

ZTEST(mpipe_latency_query, test_sink_reports_prime_and_pool)
{
	struct mpipe_aud_i2s_codec_sink k;

	zassert_ok(mpipe_aud_i2s_codec_sink_init(&k, 3));
	k.frame_interval = 10000U;
	k.mem_slab = NULL; /* no slab -> max falls back to prime */

	struct mpipe_latency_bound b = {0};

	k.sink.element.report_latency((struct mpipe_element *)&k, &b);
	zassert_equal(b.min_us, 3U * 10000U, "min: got %u", b.min_us); /* PRIME=3 */
	zassert_equal(b.max_us, 3U * 10000U, "max: got %u", b.max_us);
}

ZTEST_SUITE(mpipe_latency_query, NULL, NULL, NULL, NULL, NULL);
