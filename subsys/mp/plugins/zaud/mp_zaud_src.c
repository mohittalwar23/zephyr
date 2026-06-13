/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>

#include <zephyr/logging/log.h>

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>
#include <zephyr/mp/zaud/mp_zaud_buffer_pool.h>
#include <zephyr/mp/zaud/mp_zaud_src.h>

LOG_MODULE_REGISTER(mp_zaud_src, CONFIG_MP_LOG_LEVEL);

static struct mp_caps *mp_zaud_src_get_caps(struct mp_src *src)
{
	struct mp_zaud_src *zaud_src = MP_ZAUD_SRC(src);
	struct mp_zaud_buffer_pool *pool = MP_ZAUD_BUFFER_POOL(src->pool);
	struct audio_caps audio;
	struct mp_value *sample_rates;
	struct mp_value *bit_widths;
	struct mp_caps *caps;

	if (zaud_src->get_audio_caps == NULL || pool == NULL || pool->zaud_dev == NULL) {
		LOG_ERR("Audio capabilities or device not configured");
		return NULL;
	}

	if (zaud_src->get_audio_caps(pool->zaud_dev, &audio) != 0) {
		LOG_ERR("Failed to get audio capabilities");
		return NULL;
	}

	sample_rates =
		mp_zaud_mask_to_value_list(audio.supported_sample_rates, audio2mp_sample_rate);
	bit_widths = mp_zaud_mask_to_value_list(audio.supported_bit_widths, audio2mp_bit_width);

	caps = mp_caps_new(MP_MEDIA_AUDIO_PCM,
			   MP_CAPS_SAMPLE_RATE, MP_TYPE_LIST, sample_rates,
			   MP_CAPS_BITWIDTH, MP_TYPE_LIST, bit_widths,
			   MP_CAPS_NUM_OF_CHANNEL, MP_TYPE_UINT_RANGE, audio.min_total_channels,
			   audio.max_total_channels, 1,
			   MP_CAPS_FRAME_INTERVAL, MP_TYPE_UINT_RANGE, audio.min_frame_interval,
			   audio.max_frame_interval, 1,
			   MP_CAPS_BUFFER_COUNT, MP_TYPE_UINT_RANGE, audio.min_num_buffers,
			   UINT8_MAX, 1,
			   MP_CAPS_INTERLEAVED, MP_TYPE_BOOLEAN, audio.interleaved,
			   MP_CAPS_END);

	return caps;
}

static int mp_zaud_src_set_caps(struct mp_src *src, struct mp_caps *caps)
{
	mp_caps_replace(&src->srcpad.caps, caps);
	return 0;
}

void mp_zaud_src_init(struct mp_element *self)
{
	struct mp_src *src = (struct mp_src *)self;
	struct mp_zaud_src *zaud_src = MP_ZAUD_SRC(self);

	/* Init base class */
	mp_src_init(self);

	src->get_caps = mp_zaud_src_get_caps;
	src->set_caps = mp_zaud_src_set_caps;

	zaud_src->get_audio_caps = NULL;
}
