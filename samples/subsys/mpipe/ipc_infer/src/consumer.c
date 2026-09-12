/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The DSP half of the pipeline:
 *
 *   ipc_src -> infer_sink
 *
 * The trunk of this pipeline is on the other core. What arrives here are the
 * same buffers the M7 captured, read in place out of shared memory rather than
 * copied across.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_plugin.h>
#include <zephyr/mpipe/mpipe_pipeline.h>
#include <zephyr/mpipe/utils/mpipe_player.h>

#include "consumer.h"
#include "infer_sink.h"

LOG_MODULE_DECLARE(mpipe_ipc_infer, LOG_LEVEL_INF);

enum {
	PIPE_ID = 0,
	IPC_SRC_ID,
	INFER_SINK_ID,
};

/* Matches what the peer's caps filter pins. */
#define CONSUMER_RATE_HZ  16000
#define CONSUMER_CHANNELS 2

static struct mpipe pipe;
static struct mpipe_ipc_src source;
static struct infer_sink infer;
static struct mpipe_player player;

int consumer_start(const struct device *ipc, infer_result_cb on_result)
{
	struct mpipe_structure caps;
	int ret;

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		goto err;
	}

	/* Same endpoint name as the peer's sink; that pairing is the link. */
	ret = mpipe_ipc_src_init(&source, IPC_SRC_ID, ipc, "mpipe.audio");
	if (ret < 0) {
		goto err;
	}
	ret = infer_sink_init(&infer, INFER_SINK_ID, CONSUMER_CHANNELS, on_result);
	if (ret < 0) {
		goto err;
	}

	/*
	 * The format is told to both halves rather than negotiated across the
	 * link; the plugin cannot carry a negotiation yet. Disagreeing here is
	 * silent -- every element accepts, and the audio is simply wrong.
	 */
	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					  MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT, 10000,
					  MPIPE_CAPS_NUM_OF_CHANNEL, MPIPE_TYPE_UINT,
					  CONSUMER_CHANNELS,
					  MPIPE_CAPS_SAMPLE_RATE, MPIPE_TYPE_UINT,
					  CONSUMER_RATE_HZ,
					  MPIPE_CAPS_BITWIDTH, MPIPE_TYPE_UINT, 16,
					  MPIPE_CAPS_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&source,
			    (struct mpipe_element *)&infer, NULL);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_element_link((struct mpipe_element *)&source,
				 (struct mpipe_element *)&infer, NULL);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_ipc_src_set_format(&source, &caps);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_player_init(&player, &pipe);
	if (ret < 0) {
		goto err;
	}

	LOG_INF("inference pipeline: ipc_src -> micro_speech at %u Hz", CONSUMER_RATE_HZ);
	mpipe_player_play(&player);

	/* Only now is there somewhere for an arriving buffer to go. */
	ret = mpipe_ipc_src_start(&source);
	if (ret < 0) {
		goto err;
	}

	return 0;

err:
	LOG_ERR("cannot build the inference pipeline: %d", ret);

	return ret != 0 ? ret : -EIO;
}

bool consumer_is_bound(void)
{
	return mpipe_ipc_src_is_bound(&source);
}
