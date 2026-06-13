/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp.h>
#include <zephyr/mp/core/mp_bin.h>
#include <zephyr/mp/core/mp_messages.h>
#include <zephyr/mp/core/mp_object.h>
#include <zephyr/mp/core/mp_property.h>

#include <zephyr/mp/zaud/mp_zaud_dmic_src.h>
#include <zephyr/mp/zaud/mp_zaud_gain.h>
#include <zephyr/mp/zaud/mp_zaud_i2s_codec_sink.h>
#include <zephyr/mp/zaud/mp_zaud_property.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define PIPE_ID        0
#define DMIC_SRC_ID    1
#define CAPS_FILTER_ID 2
#define GAIN_ID        3
#define I2S_SINK_ID    4

/* Capture this many frames then stop the pipeline (deterministic EOS) */
#define NUM_FRAMES        CONFIG_AUDIO_PIPELINE_NUM_FRAMES
/* Audio frame interval in microseconds (10 ms) */
#define FRAME_INTERVAL_US 10000
/* Margin to let queued playback buffers drain before stopping */
#define DRAIN_MARGIN_MS   200

static struct mp_pipeline pipe;
static struct mp_zaud_dmic_src source;
static struct mp_zaud_gain gain;
static struct mp_zaud_i2s_codec_sink sink;
#if defined(CONFIG_MP_CAPSFILTER)
static struct mp_caps_filter caps_filter;
#endif

int main(void)
{
	int gain_val = CONFIG_AUDIO_PIPELINE_GAIN_PERCENT;
	int ret;

	MP_ELEMENT_INIT(&pipe, mp_pipeline_init, PIPE_ID);
	MP_ELEMENT_INIT(&source, mp_zaud_dmic_src_init, DMIC_SRC_ID);
	MP_ELEMENT_INIT(&gain, mp_zaud_gain_init, GAIN_ID);
	MP_ELEMENT_INIT(&sink, mp_zaud_i2s_codec_sink_init, I2S_SINK_ID);

	/* Stop after a fixed number of frames so the pipeline ends deterministically */
	ret = mp_object_set_properties(MP_OBJECT(&source), PROP_NUM_BUFS,
				       (void *)(uintptr_t)NUM_FRAMES, PROP_LIST_END);
	if (ret < 0) {
		LOG_ERR("Failed to set source properties");
		return 0;
	}

	ret = mp_object_set_properties(MP_OBJECT(&gain), PROP_GAIN, &gain_val, PROP_LIST_END);
	if (ret < 0) {
		LOG_ERR("Failed to set gain properties");
		return 0;
	}

#if defined(CONFIG_MP_CAPSFILTER)
	MP_ELEMENT_INIT(&caps_filter, mp_caps_filter_init, CAPS_FILTER_ID);

	uint32_t frame_interval = FRAME_INTERVAL_US;
	struct mp_caps *caps = mp_caps_new(MP_MEDIA_AUDIO_PCM, MP_CAPS_FRAME_INTERVAL, MP_TYPE_UINT,
					   frame_interval, MP_CAPS_END);

	if (caps == NULL) {
		LOG_ERR("Failed to create caps");
		return 0;
	}

	ret = mp_object_set_properties(MP_OBJECT(&caps_filter), PROP_CAPS, caps, PROP_LIST_END);
	mp_caps_unref(caps);
	if (ret < 0) {
		LOG_ERR("Failed to set caps filter properties");
		return 0;
	}
#endif

	if (!mp_bin_add(MP_BIN(&pipe),
			MP_ELEMENT(&source),
			IF_ENABLED(CONFIG_MP_CAPSFILTER, (MP_ELEMENT(&caps_filter),))
			MP_ELEMENT(&gain),
			MP_ELEMENT(&sink), NULL)) {
		LOG_ERR("Failed to add elements to the pipeline");
		return 0;
	}

	if (!mp_element_link(MP_ELEMENT(&source),
			IF_ENABLED(CONFIG_MP_CAPSFILTER, (MP_ELEMENT(&caps_filter),))
			MP_ELEMENT(&gain),
			MP_ELEMENT(&sink), NULL)) {
		LOG_ERR("Failed to link elements");
		return 0;
	}

	LOG_INF("Starting audio pipeline (%d frames)", NUM_FRAMES);

	if (mp_element_set_state(MP_ELEMENT(&pipe), MP_STATE_PLAYING) != MP_STATE_CHANGE_SUCCESS) {
		LOG_ERR("Failed to start pipeline");
		return 0;
	}

	/* Block until the pipeline reports end-of-stream or an error */
	struct mp_bus *bus = mp_element_get_bus(MP_ELEMENT(&pipe));
	struct mp_message *msg = mp_bus_pop_msg(bus, MP_MESSAGE_ERROR | MP_MESSAGE_EOS);

	if (msg != NULL) {
		switch (msg->type) {
		case MP_MESSAGE_EOS:
			LOG_INF("EOS received from element %d", msg->src->id);
			break;
		case MP_MESSAGE_ERROR:
			LOG_ERR("ERROR received from element %d", msg->src->id);
			break;
		default:
			break;
		}
		mp_message_destroy(msg);
	}

	/*
	 * Pause first (this also flushes any sink buffers queued by a stream too
	 * short to have hit the playback start threshold), let them drain, then
	 * stop the pipeline cleanly.
	 */
	mp_element_set_state(MP_ELEMENT(&pipe), MP_STATE_PAUSED);
	k_msleep(DRAIN_MARGIN_MS);

	if (mp_element_set_state(MP_ELEMENT(&pipe), MP_STATE_READY) != MP_STATE_CHANGE_SUCCESS) {
		LOG_ERR("Failed to stop pipeline cleanly");
		return 0;
	}

	LOG_INF("Pipeline stopped");

	return 0;
}
