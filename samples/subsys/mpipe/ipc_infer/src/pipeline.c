/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The capture half of the demo, as a real pipeline:
 *
 *   i2s_src -> caps_filter -> tee -+-> gain -> i2s_codec_sink   (audible)
 *                                  +-> ipc_sink -> ring -> HiFi4
 *
 * The audible branch is not decoration. Keyword spotting can only tell you
 * whether the model agreed with you; hearing the same audio tells you whether
 * the microphone, the clocks and the gain are doing anything at all, which is a
 * different question and the one that fails first.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/mpipe/aud/mpipe_aud_gain.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_codec_sink.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_src.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>
#include <zephyr/mpipe/base/mpipe_tee.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_plugin.h>
#include <zephyr/mpipe/mpipe_pipeline.h>
#include <zephyr/mpipe/utils/mpipe_player.h>

#include "producer.h"

LOG_MODULE_DECLARE(mpipe_ipc_infer, LOG_LEVEL_INF);

enum {
	PIPE_ID = 0,
	I2S_SRC_ID,
	CAPS_FILTER_ID,
	TEE_ID,
	GAIN_ID,
	I2S_SINK_ID,
	IPC_SINK_ID,
};

/* Shared by the source and the audible sink, as in the loopback sample. */
__nocache struct k_mem_slab mem_slab;

static struct mpipe pipe;
static struct mpipe_aud_i2s_src source;
static struct mpipe_caps_filter caps_filter;
static struct mpipe_tee tee;
static struct mpipe_aud_gain gain;
static struct mpipe_aud_i2s_codec_sink audible;
static struct mpipe_ipc_sink forward;
static struct mpipe_player player;

int producer_start(const struct device *ipc)
{
	int gain_percent = CONFIG_SAMPLE_IPC_INFER_GAIN_PERCENT;
	struct mpipe_structure caps;
	int ret;

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_aud_i2s_src_init(&source, I2S_SRC_ID,
				     DEVICE_DT_GET(DT_ALIAS(i2s_codec_rx)));
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_caps_filter_init(&caps_filter, CAPS_FILTER_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_tee_init(&tee, TEE_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_aud_gain_init(&gain, GAIN_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_aud_i2s_codec_sink_init(&audible, I2S_SINK_ID);
	if (ret < 0) {
		goto err;
	}
	/*
	 * The peer's source binds to this same endpoint name. Buffers cross by
	 * reference, so the pool feeding this sink has to live in memory the
	 * HiFi4 can address -- see the sample README.
	 */
	ret = mpipe_ipc_sink_init(&forward, IPC_SINK_ID, ipc, "mpipe.audio");
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&source,
					  MPIPE_PROP_AUD_SRC_SLAB_PTR, &mem_slab,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&audible,
					  MPIPE_PROP_AUD_SINK_SLAB_PTR, &mem_slab,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&gain,
					  MPIPE_PROP_AUD_TRANSFORM_GAIN, &gain_percent,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	/*
	 * Pin the format rather than negotiate it. The peer sized its ring
	 * periods before this pipeline existed, and micro_speech is trained at
	 * 16 kHz; a negotiation that landed on 48 kHz would satisfy every
	 * element here and still be the wrong audio.
	 */
	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					  MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT, 10000,
					  MPIPE_CAPS_NUM_OF_CHANNEL, MPIPE_TYPE_UINT, 2,
					  MPIPE_CAPS_SAMPLE_RATE, MPIPE_TYPE_UINT,
					  CONFIG_SAMPLE_IPC_INFER_RATE,
					  MPIPE_CAPS_END);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					  MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&source,
			    (struct mpipe_element *)&caps_filter,
			    (struct mpipe_element *)&tee, (struct mpipe_element *)&gain,
			    (struct mpipe_element *)&audible,
			    (struct mpipe_element *)&forward, NULL);
	if (ret < 0) {
		goto err;
	}

	/* Trunk, then the two branches. Each link takes the tee's next free pad. */
	ret = mpipe_element_link((struct mpipe_element *)&source,
				 (struct mpipe_element *)&caps_filter,
				 (struct mpipe_element *)&tee, NULL);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_element_link((struct mpipe_element *)&tee,
				 (struct mpipe_element *)&gain,
				 (struct mpipe_element *)&audible, NULL);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_element_link((struct mpipe_element *)&tee,
				 (struct mpipe_element *)&forward, NULL);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_player_init(&player, &pipe);
	if (ret < 0) {
		goto err;
	}

	LOG_INF("capture pipeline: i2s -> tee -> [speaker, HiFi4] at %u Hz",
		CONFIG_SAMPLE_IPC_INFER_RATE);
	mpipe_player_play(&player);

	return 0;

err:
	LOG_ERR("cannot build the capture pipeline: %d", ret);

	return ret != 0 ? ret : -EIO;
}

uint32_t producer_dropped(void)
{
	return forward.dropped;
}

/*
 * The mirror of the consumer's telemetry: with no console on either core that
 * can be relied on, shared memory is how both halves are observed at once.
 */
struct producer_telemetry {
	uint32_t dropped;
	uint32_t bound;
	uint32_t have_caps;
};

#define PRODUCER_TELEMETRY_ADDR (DT_REG_ADDR(DT_NODELABEL(mpipe_ipc_ctrl)) + 0x240U)

void producer_publish(void)
{
	volatile struct producer_telemetry *t =
		(volatile struct producer_telemetry *)PRODUCER_TELEMETRY_ADDR;

	t->dropped = forward.dropped;
	t->bound = mpipe_ipc_sink_is_bound(&forward) ? 1U : 0U;
	t->have_caps = forward.have_caps ? 1U : 0U;
}
