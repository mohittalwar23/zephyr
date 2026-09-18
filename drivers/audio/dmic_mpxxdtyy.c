/*
 * Copyright (c) 2018 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_mpxxdtyy

#include <string.h>

#include <zephyr/audio/audio_caps.h>
#include <zephyr/devicetree.h>

#include "dmic_mpxxdtyy.h"

#define LOG_LEVEL CONFIG_AUDIO_DMIC_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mpxxdtyy);

#define CHANNEL_MASK	0x55

static uint8_t ch_demux[128] = {
  0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x02, 0x03,
  0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x02, 0x03,
  0x04, 0x05, 0x04, 0x05, 0x06, 0x07, 0x06, 0x07,
  0x04, 0x05, 0x04, 0x05, 0x06, 0x07, 0x06, 0x07,
  0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x02, 0x03,
  0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x02, 0x03,
  0x04, 0x05, 0x04, 0x05, 0x06, 0x07, 0x06, 0x07,
  0x04, 0x05, 0x04, 0x05, 0x06, 0x07, 0x06, 0x07,
  0x08, 0x09, 0x08, 0x09, 0x0a, 0x0b, 0x0a, 0x0b,
  0x08, 0x09, 0x08, 0x09, 0x0a, 0x0b, 0x0a, 0x0b,
  0x0c, 0x0d, 0x0c, 0x0d, 0x0e, 0x0f, 0x0e, 0x0f,
  0x0c, 0x0d, 0x0c, 0x0d, 0x0e, 0x0f, 0x0e, 0x0f,
  0x08, 0x09, 0x08, 0x09, 0x0a, 0x0b, 0x0a, 0x0b,
  0x08, 0x09, 0x08, 0x09, 0x0a, 0x0b, 0x0a, 0x0b,
  0x0c, 0x0d, 0x0c, 0x0d, 0x0e, 0x0f, 0x0e, 0x0f,
  0x0c, 0x0d, 0x0c, 0x0d, 0x0e, 0x0f, 0x0e, 0x0f
};

static uint8_t left_channel(uint8_t a, uint8_t b)
{
	return ch_demux[a & CHANNEL_MASK] | (ch_demux[b & CHANNEL_MASK] << 4);
}

static uint8_t right_channel(uint8_t a, uint8_t b)
{
	a >>= 1;
	b >>= 1;
	return ch_demux[a & CHANNEL_MASK] | (ch_demux[b & CHANNEL_MASK] << 4);
}

uint16_t sw_filter_lib_init(const struct device *dev, struct dmic_cfg *cfg)
{
	struct mpxxdtyy_data *const data = dev->data;
	const struct mpxxdtyy_config *const config = dev->config;
	TPDMFilter_InitStruct *pdm_filter = &data->pdm_filter[0];
	uint16_t factor;
	uint32_t audio_freq = cfg->streams->pcm_rate;
	int i;

	/*
	 * The line carries one bitstream per microphone, so the clock follows
	 * how many are wired, not how many channels the caller wants.
	 */
	for (factor = 64U; factor <= 128U; factor += 64U) {
		uint32_t pdm_bit_clk = audio_freq * factor * config->mic_count;

		if (pdm_bit_clk >= cfg->io.min_pdm_clk_freq &&
		    pdm_bit_clk <= cfg->io.max_pdm_clk_freq) {
			break;
		}
	}

	if (factor != 64U && factor != 128U) {
		return 0;
	}

	for (i = 0; i < config->mic_count; i++) {
		/* init the filter lib */
		pdm_filter[i].LP_HZ = audio_freq / 2U;
		pdm_filter[i].HP_HZ = 10;
		pdm_filter[i].Fs = audio_freq;
		pdm_filter[i].Out_MicChannels = config->mic_count;
		pdm_filter[i].In_MicChannels = config->mic_count;
		pdm_filter[i].Decimation = factor;
		pdm_filter[i].MaxVolume = 64;

		Open_PDM_Filter_Init(&data->pdm_filter[i]);
	}

	return factor;
}

int sw_filter_lib_run(TPDMFilter_InitStruct *pdm_filter, void *pdm_block, void *pcm_block,
		      size_t pdm_size, size_t pcm_size, uint8_t out_chan)
{
	int i, j;
	int pdm_offset;
	uint8_t a, b;
	uint8_t mics;
	size_t frames;

	if (pdm_block == NULL || pcm_block == NULL || pdm_filter == NULL) {
		return -EINVAL;
	}

	mics = pdm_filter[0].In_MicChannels;
	/*
	 * Fewer channels than microphones would mix them together, which this
	 * does not do, and the decimation would write past a block sized for
	 * the smaller count.
	 */
	if ((out_chan == 0U) || (mics == 0U) || (out_chan < mics)) {
		return -EINVAL;
	}

	frames = (pcm_size / 2U) / out_chan;

	for (i = 0; i < pdm_size/2; i++) {
		switch (pdm_filter[0].In_MicChannels) {
		case 1: /* MONO */
			((uint16_t *)pdm_block)[i] = HTONS(((uint16_t *)pdm_block)[i]);
			break;

		case 2: /* STEREO */
			if (pdm_filter[0].In_MicChannels > 1) {
				a = ((uint8_t *)pdm_block)[2*i];
				b = ((uint8_t *)pdm_block)[2*i + 1];

				((uint8_t *)pdm_block)[2*i] = left_channel(a, b);
				((uint8_t *)pdm_block)[2*i + 1] = right_channel(a, b);
			}
			break;

		default:
			return -EINVAL;
		}
	}

	/*
	 * Each call produces Fs/1000 samples per channel, so the interleaved
	 * output advances by that times the channel count. The PDM bytes
	 * consumed to reach interleaved sample j are j times the decimation
	 * factor over the bits in a byte; the channel count is already in j.
	 */
	const uint32_t step = (pdm_filter[0].Fs / 1000U) * mics;

	for (j = 0; j < (int)(frames * mics); j += step) {
		pdm_offset = j * (pdm_filter[0].Decimation / 8);

		for (i = 0; i < mics; i++) {
			switch (pdm_filter[0].Decimation) {
			case 64:
				Open_PDM_Filter_64(&((uint8_t *) pdm_block)[pdm_offset + i],
						&((uint16_t *) pcm_block)[j + i],
						pdm_filter->MaxVolume,
						&pdm_filter[i]);
				break;

			case 128:
				Open_PDM_Filter_128(&((uint8_t *) pdm_block)[pdm_offset + i],
						&((uint16_t *) pcm_block)[j + i],
						pdm_filter->MaxVolume,
						&pdm_filter[i]);
				break;

			default:
				return -EINVAL;
			}
		}
	}

	/*
	 * The filter wrote one sample per microphone per frame at the front of
	 * the block. Spread those over the channels the caller asked for,
	 * walking backwards so the expansion does not overwrite samples it has
	 * still to read. A single microphone is heard on every channel, which
	 * is what a controller with one bitstream per channel gives for free.
	 */
	if (out_chan > mics) {
		int16_t *pcm = pcm_block;

		for (j = (int)frames - 1; j >= 0; j--) {
			for (i = out_chan - 1; i >= 0; i--) {
				pcm[j * out_chan + i] = pcm[j * mics + (i % mics)];
			}
		}
	}

	return 0;
}

static int mpxxdtyy_get_caps(const struct device *dev, struct audio_caps *caps)
{
	ARG_UNUSED(dev);

	memset(caps, 0, sizeof(*caps));
	caps->min_total_channels = 1U;
	caps->max_total_channels = 2U;
	/*
	 * Which rates are reachable depends on the oversampling factor and the
	 * channel count, which are not known until configure() picks them, so
	 * it rejects what the microphone's PDM clock range cannot meet.
	 */
	caps->supported_sample_rates = AUDIO_SAMPLE_RATE_8000 | AUDIO_SAMPLE_RATE_16000 |
				       AUDIO_SAMPLE_RATE_22050 | AUDIO_SAMPLE_RATE_32000 |
				       AUDIO_SAMPLE_RATE_44100 | AUDIO_SAMPLE_RATE_48000;
	/* The decimation filter writes 16-bit samples and nothing else. */
	caps->supported_bit_widths = AUDIO_BIT_WIDTH_16;
	/* One block is allocated per read and handed straight to the caller. */
	caps->min_num_buffers = 1U;
	caps->min_frame_interval = 1000U;
	caps->max_frame_interval = 100000U;
	caps->interleaved = true;

	return 0;
}

static DEVICE_API(dmic, mpxxdtyy_driver_api) = {
#if DT_ANY_INST_ON_BUS_STATUS_OKAY(i2s)
	.configure = mpxxdtyy_i2s_configure,
	.trigger = mpxxdtyy_i2s_trigger,
	.read = mpxxdtyy_i2s_read,
#endif /* DT_ANY_INST_ON_BUS_STATUS_OKAY(i2s) */
	.get_caps = mpxxdtyy_get_caps,
};

static int mpxxdtyy_initialize(const struct device *dev)
{
	const struct mpxxdtyy_config *config = dev->config;
	struct mpxxdtyy_data *const data = dev->data;

	if (!device_is_ready(config->comm_master)) {
		return -ENODEV;
	}

	data->state = DMIC_STATE_INITIALIZED;
	return 0;
}

static const struct mpxxdtyy_config mpxxdtyy_config = {
	.comm_master = DEVICE_DT_GET(DT_INST_BUS(0)),
	.mic_count = DT_INST_PROP(0, mic_count),
};

static struct mpxxdtyy_data mpxxdtyy_data;

DEVICE_DT_INST_DEFINE(0, mpxxdtyy_initialize, NULL, &mpxxdtyy_data,
		      &mpxxdtyy_config, POST_KERNEL,
		      CONFIG_AUDIO_DMIC_INIT_PRIORITY, &mpxxdtyy_driver_api);
