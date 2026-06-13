/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/audio/codec.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_buffer.h>
#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_pad.h>
#include <zephyr/mp/core/mp_sink.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>
#include <zephyr/mp/zaud/mp_zaud_i2s_codec_sink.h>

LOG_MODULE_REGISTER(mp_zaud_i2s_codec_sink, CONFIG_MP_LOG_LEVEL);

#if !DT_NODE_EXISTS(DT_ALIAS(i2s_node0))
#error "i2s-node0 devicetree alias not found. Add it to your board overlay."
#endif
#if !DT_NODE_EXISTS(DT_NODELABEL(audio_codec))
#error "audio_codec devicetree node label not found. Add it to your board overlay."
#endif

/* TODO: devices are hardcoded via DT alias/nodelabel, limiting this element to a
 * single instance. Make them per-instance properties to support multiple sinks.
 */
#define ZAUD_I2S_DEV        DEVICE_DT_GET(DT_ALIAS(i2s_node0))
#define ZAUD_CODEC_DEV      DEVICE_DT_GET(DT_NODELABEL(audio_codec))
#define ZAUD_I2S_NUM_BLOCKS CONFIG_MP_PLUGIN_ZAUD_NUM_BLOCKS
#define ZAUD_I2S_MAX_FRAME  CONFIG_MP_PLUGIN_ZAUD_MAX_FRAME_SIZE
#define ZAUD_I2S_ALLOC_TIMEOUT_MS 1000
/*
 * Prime the TX queue with several blocks before starting the stream. The
 * native_sim I2S worker treats a momentarily-empty queue while running as a
 * fatal underrun, so a startup cushion keeps the stream fed once the producer
 * and consumer run at the same (paced) rate.
 */
#define ZAUD_I2S_PREBUFFER  (ZAUD_I2S_NUM_BLOCKS / 2)

/* DMA-capable backing for the I2S driver's TX mem slab */
static char __nocache __aligned(sizeof(void *))
	zaud_i2s_slab_backing[ZAUD_I2S_NUM_BLOCKS * ZAUD_I2S_MAX_FRAME];
static struct k_mem_slab zaud_i2s_slab;

static struct mp_caps *mp_zaud_i2s_codec_sink_get_caps(struct mp_sink *sink)
{
	struct mp_zaud_i2s_codec_sink *self = MP_ZAUD_I2S_CODEC_SINK(sink);
	struct audio_caps i2s_caps;
	struct audio_caps codec_caps;
	struct mp_value *sample_rates;
	struct mp_value *bit_widths;
	uint32_t rates;
	uint32_t widths;

	if (i2s_get_caps(self->i2s_dev, &i2s_caps, I2S_DIR_TX) != 0 ||
	    audio_codec_get_caps(self->codec_dev, &codec_caps) != 0) {
		LOG_ERR("Failed to get I2S/codec capabilities");
		return NULL;
	}

	if (i2s_caps.interleaved != codec_caps.interleaved) {
		LOG_ERR("Interleaved capability mismatch between I2S and codec");
		return NULL;
	}

	rates = i2s_caps.supported_sample_rates & codec_caps.supported_sample_rates;
	widths = i2s_caps.supported_bit_widths & codec_caps.supported_bit_widths;
	sample_rates = mp_zaud_mask_to_value_list(rates, audio2mp_sample_rate);
	bit_widths = mp_zaud_mask_to_value_list(widths, audio2mp_bit_width);

	return mp_caps_new(MP_MEDIA_AUDIO_PCM,
			   MP_CAPS_SAMPLE_RATE, MP_TYPE_LIST, sample_rates,
			   MP_CAPS_BITWIDTH, MP_TYPE_LIST, bit_widths,
			   MP_CAPS_NUM_OF_CHANNEL, MP_TYPE_UINT_RANGE,
			   MAX(i2s_caps.min_total_channels, codec_caps.min_total_channels),
			   MIN(i2s_caps.max_total_channels, codec_caps.max_total_channels), 1,
			   MP_CAPS_FRAME_INTERVAL, MP_TYPE_UINT_RANGE,
			   MAX(i2s_caps.min_frame_interval, codec_caps.min_frame_interval),
			   MIN(i2s_caps.max_frame_interval, codec_caps.max_frame_interval), 1,
			   MP_CAPS_BUFFER_COUNT, MP_TYPE_UINT_RANGE,
			   MAX(i2s_caps.min_num_buffers, codec_caps.min_num_buffers), UINT8_MAX, 1,
			   MP_CAPS_INTERLEAVED, MP_TYPE_BOOLEAN, codec_caps.interleaved,
			   MP_CAPS_END);
}

static bool mp_zaud_i2s_codec_sink_set_caps(struct mp_sink *sink, struct mp_caps *caps)
{
	struct mp_zaud_i2s_codec_sink *self = MP_ZAUD_I2S_CODEC_SINK(sink);
	struct mp_structure *s = mp_caps_get_structure(caps, 0);
	struct i2s_config config = {0};
	struct audio_codec_cfg audio_cfg = {0};
	uint32_t block_size;
	uint32_t slab_block;

	int sample_rate = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_SAMPLE_RATE));
	int bit_width = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_BITWIDTH));
	int num_of_channel = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_NUM_OF_CHANNEL));
	int frame_interval = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_FRAME_INTERVAL));

	block_size = mp_zaud_frame_size(bit_width, sample_rate, frame_interval, num_of_channel);
	slab_block = ROUND_UP(block_size, sizeof(void *));

	if (slab_block == 0 || slab_block > ZAUD_I2S_MAX_FRAME) {
		LOG_ERR("Frame size %u exceeds max %u", block_size, ZAUD_I2S_MAX_FRAME);
		return false;
	}

	if (k_mem_slab_init(&zaud_i2s_slab, zaud_i2s_slab_backing, slab_block,
			    ZAUD_I2S_NUM_BLOCKS) < 0) {
		LOG_ERR("Failed to init I2S TX mem slab");
		return false;
	}
	self->mem_slab = &zaud_i2s_slab;

	/* mclk_freq is board/codec-derived; left 0 (the dummy codec ignores it). */
	audio_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	audio_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	audio_cfg.dai_cfg.i2s.word_size = bit_width;
	audio_cfg.dai_cfg.i2s.channels = num_of_channel;
	audio_cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
	audio_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	audio_cfg.dai_cfg.i2s.frame_clk_freq = sample_rate;
	audio_cfg.dai_cfg.i2s.mem_slab = self->mem_slab;
	audio_cfg.dai_cfg.i2s.block_size = block_size;
	if (audio_codec_configure(self->codec_dev, &audio_cfg) < 0) {
		LOG_ERR("Failed to configure codec");
		return false;
	}

	config.word_size = bit_width;
	config.channels = num_of_channel;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
	config.frame_clk_freq = sample_rate;
	config.mem_slab = self->mem_slab;
	config.block_size = block_size;
	config.timeout = frame_interval * 10;

	if (i2s_configure(self->i2s_dev, I2S_DIR_TX, &config) < 0) {
		LOG_ERR("Failed to configure I2S TX stream");
		return false;
	}

	mp_caps_replace(&sink->sinkpad.caps, caps);

	return true;
}

static bool mp_zaud_i2s_codec_sink_chainfn(struct mp_pad *pad, struct net_buf *in_buf,
					   struct net_buf **out_buf)
{
	struct mp_zaud_i2s_codec_sink *self = MP_ZAUD_I2S_CODEC_SINK(pad->object.container);
	void *block;
	int ret;

	if (in_buf == NULL || in_buf->data == NULL || in_buf->len == 0) {
		LOG_ERR("Invalid buffer received");
		return false;
	}

	/*
	 * The I2S driver owns its TX buffers through a private mem slab and frees
	 * each block after transmitting it, so copy the transport buffer into a
	 * freshly allocated slab block (see issue #107868 for the longer-term
	 * RTIO-based zero-copy plan).
	 */
	ret = k_mem_slab_alloc(self->mem_slab, &block, K_MSEC(ZAUD_I2S_ALLOC_TIMEOUT_MS));
	if (ret < 0) {
		LOG_ERR("Failed to allocate I2S TX block: %d", ret);
		return false;
	}

	memcpy(block, in_buf->data, in_buf->len);

	ret = i2s_write(self->i2s_dev, block, in_buf->len);
	if (ret < 0) {
		LOG_ERR("Failed to write I2S data: %d", ret);
		k_mem_slab_free(self->mem_slab, block);
		return false;
	}

	/* Start the stream once enough blocks are queued to avoid startup underrun */
	if (!self->started) {
		self->count++;
		if (self->count >= ZAUD_I2S_PREBUFFER) {
			i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			self->started = true;
		}
	}

	/* Sink consumes the buffer; nothing flows downstream */
	net_buf_unref(in_buf);
	*out_buf = NULL;

	return true;
}

static enum mp_state_change_return
mp_zaud_i2s_codec_sink_change_state(struct mp_element *self, enum mp_state_change transition)
{
	struct mp_zaud_i2s_codec_sink *codec_sink = MP_ZAUD_I2S_CODEC_SINK(self);

	/*
	 * This intentionally fully overrides the base mp_sink change_state (a no-op
	 * TODO today). If the base ever gains real teardown logic, delegate to it
	 * from the default case instead of returning SUCCESS unconditionally.
	 */
	switch (transition) {
	case MP_STATE_CHANGE_READY_TO_PAUSED:
		/* Fail fast with a clear cause rather than negotiating a dead sink. */
		if (!device_is_ready(codec_sink->i2s_dev) ||
		    !device_is_ready(codec_sink->codec_dev)) {
			LOG_ERR("I2S/codec device not ready");
			return MP_STATE_CHANGE_FAILURE;
		}
		break;
	case MP_STATE_CHANGE_PLAYING_TO_PAUSED:
		/*
		 * A stream shorter than the prebuffer threshold never reaches the
		 * count that triggers I2S_TRIGGER_START, leaving its blocks stuck in
		 * the queue. On stop, start the stream so they are flushed/played.
		 */
		if (!codec_sink->started && codec_sink->count > 0) {
			i2s_trigger(codec_sink->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			codec_sink->started = true;
		}
		break;
	case MP_STATE_CHANGE_PAUSED_TO_READY:
		/* Reset playback state so a subsequent run re-primes the prebuffer. */
		codec_sink->started = false;
		codec_sink->count = 0;
		break;
	default:
		break;
	}

	return MP_STATE_CHANGE_SUCCESS;
}

void mp_zaud_i2s_codec_sink_init(struct mp_element *self)
{
	struct mp_sink *sink = MP_SINK(self);
	struct mp_zaud_i2s_codec_sink *codec_sink = MP_ZAUD_I2S_CODEC_SINK(self);

	/* Init base class */
	mp_sink_init(self);

	codec_sink->i2s_dev = ZAUD_I2S_DEV;
	codec_sink->codec_dev = ZAUD_CODEC_DEV;
	codec_sink->mem_slab = NULL;
	codec_sink->started = false;
	codec_sink->count = 0;

	/*
	 * Wire the element's function pointers unconditionally, before the
	 * device-readiness checks. If they were skipped on a not-ready device,
	 * change_state would keep the inherited no-op (making the READY_TO_PAUSED
	 * fail-fast below dead code) and chainfn would stay NULL, so buffers
	 * reaching this sink would be silently dropped without an unref, leaking
	 * the net_buf pool until the pipeline hangs. With them wired, a not-ready
	 * device fails loudly via the change_state guard instead.
	 */
	self->change_state = mp_zaud_i2s_codec_sink_change_state;
	sink->sinkpad.chainfn = mp_zaud_i2s_codec_sink_chainfn;
	sink->get_caps = mp_zaud_i2s_codec_sink_get_caps;
	sink->set_caps = mp_zaud_i2s_codec_sink_set_caps;

	if (!device_is_ready(codec_sink->i2s_dev)) {
		LOG_ERR("%s is not ready", codec_sink->i2s_dev->name);
		return;
	}
	if (!device_is_ready(codec_sink->codec_dev)) {
		LOG_ERR("%s is not ready", codec_sink->codec_dev->name);
		return;
	}

	/* Populate supported caps from the I2S/codec devices for negotiation */
	mp_sink_update_caps(sink, sink->get_caps(sink));
}
