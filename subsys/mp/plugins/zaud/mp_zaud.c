/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>

struct mp_zaud_desc {
	uint32_t value;
	uint32_t mask;
};

static const struct mp_zaud_desc mp_zaud_sample_rates[] = {
	{MP_ZAUD_SAMPLE_RATE_8000, AUDIO_SAMPLE_RATE_8000},
	{MP_ZAUD_SAMPLE_RATE_16000, AUDIO_SAMPLE_RATE_16000},
	{MP_ZAUD_SAMPLE_RATE_32000, AUDIO_SAMPLE_RATE_32000},
	{MP_ZAUD_SAMPLE_RATE_44100, AUDIO_SAMPLE_RATE_44100},
	{MP_ZAUD_SAMPLE_RATE_48000, AUDIO_SAMPLE_RATE_48000},
	{MP_ZAUD_SAMPLE_RATE_96000, AUDIO_SAMPLE_RATE_96000},
};

static const struct mp_zaud_desc mp_zaud_bit_widths[] = {
	{MP_ZAUD_BIT_WIDTH_16, AUDIO_BIT_WIDTH_16},
	{MP_ZAUD_BIT_WIDTH_24, AUDIO_BIT_WIDTH_24},
	{MP_ZAUD_BIT_WIDTH_32, AUDIO_BIT_WIDTH_32},
};

uint32_t audio2mp_sample_rate(uint32_t sample_rate_mask)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(mp_zaud_sample_rates); i++) {
		if (mp_zaud_sample_rates[i].mask == sample_rate_mask) {
			return mp_zaud_sample_rates[i].value;
		}
	}

	return 0;
}

uint32_t audio2mp_bit_width(uint32_t bit_width_mask)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(mp_zaud_bit_widths); i++) {
		if (mp_zaud_bit_widths[i].mask == bit_width_mask) {
			return mp_zaud_bit_widths[i].value;
		}
	}

	return 0;
}

uint32_t mp_zaud_frame_size(int bit_width, int sample_rate, int frame_interval_us, int channels)
{
	/*
	 * 64-bit intermediate: sample_rate * frame_interval_us overflows int32 for
	 * large intervals (e.g. 16 kHz * 1 s = 1.6e10).
	 */
	return (uint32_t)((bit_width / BITS_PER_BYTE) *
			  (((int64_t)sample_rate * frame_interval_us) / 1000000) * channels);
}

struct mp_value *mp_zaud_mask_to_value_list(uint32_t mask, uint32_t (*conv)(uint32_t))
{
	struct mp_value *list = mp_value_new(MP_TYPE_LIST, NULL);

	for (int i = 0; mask > 0; i++, mask >>= 1) {
		if (mask & 1) {
			mp_value_list_append(list, mp_value_new(MP_TYPE_UINT, conv(BIT(i))));
		}
	}

	return list;
}
