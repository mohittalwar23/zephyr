/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_fake_src.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/mpipe/mpipe_sink.h>
#include <zephyr/mpipe/mpipe_transform.h>

ZBUS_MSG_SUBSCRIBER_DEFINE(test_pipeline_sub);

/* Element IDs (values are arbitrary; only uniqueness within the pipeline matters) */
enum {
	PIPE_ID,
	SRC_ID,
	TRANSFORM_ID,
	SINK_ID,
};

/* Number of buffers the source shall produce before EOS */
#define TEST_BUFS_NUM 10

struct test_mock_pipeline_fixture {
	struct mpipe pipeline;
	struct mpipe_fake_src fake_src;
	struct mpipe_transform transform;
	struct mpipe_sink sink;
};

struct push_pipeline {
	struct mpipe pipeline;
	struct mpipe_fake_src source;
	struct mpipe_sink sink;
};

static struct push_pipeline push_a;
static struct push_pipeline push_b;

static void push_pipeline_init(struct push_pipeline *ctx, uint8_t first_id)
{
	memset(ctx, 0, sizeof(*ctx));
	zassert_ok(mpipe_pipeline_init(&ctx->pipeline, first_id));
	zassert_ok(mpipe_fake_src_init(&ctx->source, first_id + 1U));
	zassert_ok(mpipe_sink_init(&ctx->sink, first_id + 2U));
	ctx->source.src.drive = MPIPE_SRC_DRIVE_PUSH;

	zassert_ok(mpipe_bin_add(&ctx->pipeline.bin,
				 (struct mpipe_element *)&ctx->source,
				 (struct mpipe_element *)&ctx->sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&ctx->source,
				      (struct mpipe_element *)&ctx->sink, NULL));
}

static void *pipeline_suite_setup(void)
{
	static struct test_mock_pipeline_fixture fixture;

	return &fixture;
}

static void pipeline_before(void *f)
{
	struct test_mock_pipeline_fixture *fix = f;

	memset(fix, 0, sizeof(*fix));

	zassert_ok(mpipe_pipeline_init(&fix->pipeline, PIPE_ID));
	zassert_ok(mpipe_fake_src_init(&fix->fake_src, SRC_ID));
	zassert_ok(mpipe_transform_init(&fix->transform, TRANSFORM_ID));
	zassert_ok(mpipe_sink_init(&fix->sink, SINK_ID));

	/* Set number of buffers to produce before EOS */
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&fix->fake_src,
					       MPIPE_PROP_SRC_NUM_BUFS, TEST_BUFS_NUM,
					       MPIPE_PROP_LIST_END),
		   "Failed to set fake_src MPIPE_PROP_SRC_NUM_BUFS");
}

static void pipeline_after(void *f)
{
	struct test_mock_pipeline_fixture *fix = f;
	struct zbus_channel *bus =
		mpipe_element_get_bus_chan((struct mpipe_element *)&fix->pipeline);

	/* Also runs when the test failed before detaching its own observer. */
	(void)zbus_chan_rm_obs(bus, &test_pipeline_sub, K_FOREVER);
}

ZTEST_SUITE(test_mock_pipeline, NULL, pipeline_suite_setup, pipeline_before, pipeline_after, NULL);

ZTEST_F(test_mock_pipeline, test_pipeline_fake_src_transform_sink)
{
	const struct zbus_channel *chan;
	struct zbus_channel *bus;
	struct mpipe_message msg;

	/* Add all elements to the pipeline */
	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&fixture->pipeline,
				 (struct mpipe_element *)&fixture->fake_src,
				 (struct mpipe_element *)&fixture->transform,
				 (struct mpipe_element *)&fixture->sink, NULL),
		   "Failed to add elements");

	/* Link: fake_src → transform → sink */
	zassert_ok(mpipe_element_link((struct mpipe_element *)&fixture->fake_src,
				      (struct mpipe_element *)&fixture->transform,
				      (struct mpipe_element *)&fixture->sink, NULL),
		   "Failed to link elements");

	/* Attach the subscriber to the pipeline bin's channel at runtime. */
	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&fixture->pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &test_pipeline_sub, K_FOREVER),
		   "Failed to add observer to pipeline channel");

	/*
	 * Run it more than once. A second run is where state left behind by the
	 * first one shows up - a stale end-of-stream, a buffer pool carrying the
	 * demands it was negotiated to last time - and each of those has been a
	 * real defect. One run proves the pipeline streams; the replay proves it
	 * goes back to where it started.
	 */
	for (int run = 0; run < 3; run++) {
		zassert_equal(mpipe_element_set_state((struct mpipe_element *)&fixture->pipeline,
						      MPIPE_STATE_PLAYING),
			      MPIPE_STATE_CHANGE_SUCCESS, "run %d failed to start PLAYING", run);

		/* Wait for EOS posted by the sink */
		zassert_ok(zbus_sub_wait_msg(&test_pipeline_sub, &chan, &msg, K_FOREVER),
			   "run %d timed out waiting for a pipeline message", run);
		zassert_equal(msg.type, MPIPE_MESSAGE_EOS, "run %d: expected EOS, got %d", run,
			      msg.type);

		/* Exactly one EOS per run: a second would mean the aggregation leaked */
		zassert_equal(zbus_sub_wait_msg(&test_pipeline_sub, &chan, &msg, K_MSEC(50)),
			      -ENOMSG, "run %d produced more than one message", run);

		/* Bring pipeline back to READY and join the thread */
		zassert_equal(mpipe_element_set_state((struct mpipe_element *)&fixture->pipeline,
						      MPIPE_STATE_READY),
			      MPIPE_STATE_CHANGE_SUCCESS, "run %d failed to return to READY", run);
	}

	/* Detach the runtime observer */
	zassert_ok(zbus_chan_rm_obs(bus, &test_pipeline_sub, K_FOREVER),
		   "Failed to remove observer from pipeline channel");
}

/* Push sources bring their own callback context and consume no pipeline stack. */
ZTEST(test_mock_pipeline, test_two_push_pipelines_need_no_pipeline_threads)
{
	enum mpipe_state_change_return ret_a;
	enum mpipe_state_change_return ret_b;

	push_pipeline_init(&push_a, 10U);
	push_pipeline_init(&push_b, 20U);

	ret_a = mpipe_element_set_state(&push_a.pipeline.bin.element,
					MPIPE_STATE_PLAYING);
	ret_b = mpipe_element_set_state(&push_b.pipeline.bin.element,
					MPIPE_STATE_PLAYING);

	/* Release any thread the unfixed implementation managed to allocate. */
	if (ret_a == MPIPE_STATE_CHANGE_SUCCESS) {
		zassert_equal(mpipe_element_set_state(&push_a.pipeline.bin.element,
						      MPIPE_STATE_READY),
			      MPIPE_STATE_CHANGE_SUCCESS);
	}
	if (ret_b == MPIPE_STATE_CHANGE_SUCCESS) {
		zassert_equal(mpipe_element_set_state(&push_b.pipeline.bin.element,
						      MPIPE_STATE_READY),
			      MPIPE_STATE_CHANGE_SUCCESS);
	}

	zassert_equal(ret_a, MPIPE_STATE_CHANGE_SUCCESS);
	zassert_equal(ret_b, MPIPE_STATE_CHANGE_SUCCESS,
		      "a push pipeline consumed the only pull-thread stack");
}
