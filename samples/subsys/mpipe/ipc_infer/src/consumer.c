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
	 * Seeded only so the pipeline can link before the peer attaches. The
	 * format that actually applies is the one the peer announces, which is
	 * the whole point: one negotiation covering both halves rather than two
	 * that are trusted to agree.
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

/*
 * Progress, published where Linux can read it.
 *
 * This core has no console on this board -- both cores' consoles are the same
 * UART and the M7 keeps it -- so anything that goes wrong before a result is
 * produced is otherwise invisible. The area sits past the 256 bytes the
 * transport owns, inside the same reserved page.
 */
struct consumer_telemetry {
	uint32_t buffers;
	uint32_t windows;
	uint32_t last_category;
	uint32_t refused;
	uint32_t bound;
	uint32_t playing;
};

#define CONSUMER_TELEMETRY_ADDR (DT_REG_ADDR(DT_NODELABEL(mpipe_ipc_ctrl)) + 0x200U)

void consumer_publish(void)
{
	volatile struct consumer_telemetry *t =
		(volatile struct consumer_telemetry *)CONSUMER_TELEMETRY_ADDR;

	t->buffers = infer.buffers;
	t->windows = infer.windows;
	t->last_category = infer.last_category;
	t->refused = source.refused;
	t->bound = mpipe_ipc_src_is_bound(&source) ? 1U : 0U;
	t->playing = 1U;
}
