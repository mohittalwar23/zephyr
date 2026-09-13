/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The end of the pipeline on the DSP: collect a second of audio and ask
 * micro_speech what it heard.
 *
 * Kept in the sample rather than the subsystem because the pieces it joins are
 * still moving -- the model comes from a pull request that has not landed, and
 * a model-agnostic inference element belongs in mpipe only once there is more
 * than one model to be agnostic about.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mpipe/mpipe_buffer.h>

#include "infer_sink.h"

LOG_MODULE_DECLARE(mpipe_ipc_infer, LOG_LEVEL_INF);

/* The C surface of the C++ inference code. */
int micro_speech_process_audio(const int16_t *audio_data, size_t audio_data_size);
const char *micro_speech_category_label(int category);

static int infer_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
			  struct net_buf **out_buf)
{
	struct infer_sink *infer;
	struct mpipe_buffer_meta *meta;
	const int16_t *samples;
	uint32_t frames;

	__ASSERT_NO_MSG(pad != NULL);
	__ASSERT_NO_MSG(in_buf != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	infer = CONTAINER_OF(pad->object.container, struct infer_sink,
			     base.element.object);
	meta = mpipe_buffer_get_meta(in_buf);

	*out_buf = NULL;

	infer->buffers++;
	samples = (const int16_t *)in_buf->data;
	frames = meta->bytes_used / (infer->channels * sizeof(int16_t));

	for (uint32_t i = 0; i < frames; i++) {
		if (infer->fill >= INFER_WINDOW_SAMPLES) {
			break;
		}
		/* Keep one channel: the model is mono. */
		infer->window[infer->fill++] = samples[i * infer->channels];
	}

	/*
	 * Release before inferring, not after. The buffer belongs to the peer,
	 * and holding it across fifty milliseconds of inference would stall a
	 * capture running in real time on the other core.
	 */
	net_buf_unref(in_buf);

	if (infer->fill < INFER_WINDOW_SAMPLES) {
		return 0;
	}

	infer->fill = 0U;

	{
		uint32_t started = k_cycle_get_32();
		int category = micro_speech_process_audio(infer->window,
							  INFER_WINDOW_SAMPLES);
		uint32_t ms = k_cyc_to_ms_near32(k_cycle_get_32() - started);

		if (category < 0) {
			LOG_ERR("window %u: inference failed: %d", infer->windows,
				category);
			return 0;
		}

		LOG_INF("window %u: heard '%s' in %u ms", infer->windows,
			micro_speech_category_label(category), ms);

		infer->last_category = (uint32_t)category;

		if (infer->on_result != NULL) {
			infer->on_result(infer->windows, (uint32_t)category);
		}
		infer->windows++;
	}

	return 0;
}

int infer_sink_init(struct infer_sink *infer, uint8_t id, uint32_t channels,
		    infer_result_cb on_result)
{
	int ret;

	if (infer == NULL || channels == 0U) {
		return -EINVAL;
	}

	memset(infer, 0, sizeof(*infer));

	ret = mpipe_sink_init(&infer->base, id);
	if (ret < 0) {
		return ret;
	}

	infer->channels = channels;
	infer->on_result = on_result;
	infer->base.sink_pad.chain_fn = infer_chain_fn;

	return 0;
}
