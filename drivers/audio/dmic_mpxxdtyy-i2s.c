/*
 * Copyright (c) 2018 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_mpxxdtyy

#include "dmic_mpxxdtyy.h"
#include <zephyr/drivers/i2s.h>

#define LOG_LEVEL CONFIG_AUDIO_DMIC_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(mpxxdtyy);

#if DT_ANY_INST_ON_BUS_STATUS_OKAY(i2s)

#define NUM_RX_BLOCKS			4
#define PDM_BLOCK_MAX_SIZE_BYTES        CONFIG_DMIC_MPXXDTYY_PDM_BLOCK_SIZE

K_MEM_SLAB_DEFINE(rx_pdm_i2s_mslab, PDM_BLOCK_MAX_SIZE_BYTES, NUM_RX_BLOCKS, 1);

int mpxxdtyy_i2s_read(const struct device *dev, uint8_t stream, void **buffer,
		      size_t *size, int32_t timeout)
{
	int ret;
	const struct mpxxdtyy_config *config = dev->config;
	struct mpxxdtyy_data *const data = dev->data;
	void *pdm_block, *pcm_block;
	size_t pdm_size;
	TPDMFilter_InitStruct *pdm_filter = &data->pdm_filter[0];

	ret = i2s_read(config->comm_dev, &pdm_block, &pdm_size);
	if (ret != 0) {
		/* A reader that bounds its wait so it can be joined reaches this. */
		LOG_DBG("no PDM block within the timeout: %d", ret);
		return ret;
	}

	ret = k_mem_slab_alloc(data->pcm_mem_slab,
			       &pcm_block, K_NO_WAIT);
	if (ret < 0) {
		return ret;
	}

	sw_filter_lib_run(pdm_filter, pdm_block, pcm_block, pdm_size, data->pcm_mem_size,
			  data->out_chan);
	k_mem_slab_free(&rx_pdm_i2s_mslab, pdm_block);

	*buffer = pcm_block;
	*size = data->pcm_mem_size;

	return 0;
}

int mpxxdtyy_i2s_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	int ret;
	const struct mpxxdtyy_config *config = dev->config;
	struct mpxxdtyy_data *const data = dev->data;
	enum i2s_trigger_cmd i2s_cmd;
	enum dmic_state tmp_state;

	switch (cmd) {
	case DMIC_TRIGGER_START:
		if (data->state == DMIC_STATE_CONFIGURED) {
			tmp_state = DMIC_STATE_ACTIVE;
			i2s_cmd = I2S_TRIGGER_START;
		} else {
			return 0;
		}
		break;
	case DMIC_TRIGGER_RELEASE:
		/* Resume a paused stream; the configuration is still good. */
		if (data->state == DMIC_STATE_PAUSED) {
			tmp_state = DMIC_STATE_ACTIVE;
			i2s_cmd = I2S_TRIGGER_START;
		} else {
			return 0;
		}
		break;
	case DMIC_TRIGGER_PAUSE:
		/* Stop capturing but stay configured, so RELEASE can resume. */
		if (data->state == DMIC_STATE_ACTIVE) {
			tmp_state = DMIC_STATE_PAUSED;
			i2s_cmd = I2S_TRIGGER_STOP;
		} else {
			return 0;
		}
		break;
	case DMIC_TRIGGER_STOP:
	case DMIC_TRIGGER_RESET:
		if (data->state == DMIC_STATE_ACTIVE || data->state == DMIC_STATE_PAUSED) {
			tmp_state = DMIC_STATE_CONFIGURED;
			i2s_cmd = (data->state == DMIC_STATE_ACTIVE) ? I2S_TRIGGER_STOP
								     : I2S_TRIGGER_DROP;
		} else {
			return 0;
		}
		break;
	default:
		return -EINVAL;
	}

	ret = i2s_trigger(config->comm_dev, I2S_DIR_RX, i2s_cmd);
	if (ret != 0) {
		LOG_ERR("trigger failed with %d error", ret);
		return ret;
	}

	data->state = tmp_state;
	return 0;
}

int mpxxdtyy_i2s_configure(const struct device *dev, struct dmic_cfg *cfg)
{
	int ret;
	const struct mpxxdtyy_config *config = dev->config;
	struct mpxxdtyy_data *const data = dev->data;
	uint8_t chan_size = cfg->streams->pcm_width;
	uint32_t audio_freq = cfg->streams->pcm_rate;
	uint32_t block_ms;
	uint16_t factor;

	if ((cfg->channel.req_num_chan == 0U) || (cfg->channel.req_num_chan < config->mic_count)) {
		LOG_ERR("%u channels cannot carry %u microphones", cfg->channel.req_num_chan,
			config->mic_count);
		return -EINVAL;
	}

	/* PCM buffer size */
	data->pcm_mem_slab = cfg->streams->mem_slab;
	data->pcm_mem_size = cfg->streams->block_size;
	data->out_chan = cfg->channel.req_num_chan;

	/*
	 * The caller describes what its microphone accepts. Narrow that to what
	 * this driver can produce rather than refusing it: a caller asking for
	 * a wider range than the part supports is not an error, and rejecting
	 * it turns away the values samples/drivers/audio/dmic passes.
	 */
	if (cfg->io.min_pdm_clk_freq > cfg->io.max_pdm_clk_freq) {
		return -EINVAL;
	}

	cfg->io.min_pdm_clk_freq = MAX(cfg->io.min_pdm_clk_freq, MPXXDTYY_MIN_PDM_FREQ);
	cfg->io.max_pdm_clk_freq = MIN(cfg->io.max_pdm_clk_freq, MPXXDTYY_MAX_PDM_FREQ);

	if (cfg->io.min_pdm_clk_freq > cfg->io.max_pdm_clk_freq) {
		LOG_ERR("no PDM clock in %u..%u this part can produce", cfg->io.min_pdm_clk_freq,
			cfg->io.max_pdm_clk_freq);
		return -EINVAL;
	}

	factor = sw_filter_lib_init(dev, cfg);
	if (factor == 0U) {
		return -EINVAL;
	}

	/* configure I2S channels */
	struct i2s_config i2s_cfg;

	i2s_cfg.word_size = chan_size;
	/* The link carries one bitstream per microphone, whatever the caller asked for. */
	i2s_cfg.channels = config->mic_count;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED |
			 I2S_FMT_BIT_CLK_INV;
	i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = audio_freq * factor / chan_size;
	/*
	 * One PDM bit per microphone per oversampled tick, so the bitstream is
	 * sized from the frames the PCM block holds rather than its samples.
	 */
	i2s_cfg.block_size = (data->pcm_mem_size / 2U / cfg->channel.req_num_chan) *
			     config->mic_count * (factor / 8U);
	if (i2s_cfg.block_size > PDM_BLOCK_MAX_SIZE_BYTES) {
		LOG_ERR("PDM block of %u bytes exceeds the %u the slab holds", i2s_cfg.block_size,
			PDM_BLOCK_MAX_SIZE_BYTES);
		return -EINVAL;
	}
	i2s_cfg.mem_slab = &rx_pdm_i2s_mslab;
	/*
	 * Wait a few block periods, not a flat two seconds. The link is the
	 * only thing a read blocks on, so this bounds how long a stop waits for
	 * one already in flight.
	 */
	block_ms = (data->pcm_mem_size * 1000U) /
		   (audio_freq * cfg->channel.req_num_chan * (chan_size / 8U));
	i2s_cfg.timeout = (int32_t)MAX(100U, block_ms * 10U);

	ret = i2s_configure(config->comm_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret != 0) {
		LOG_ERR("I2S device configuration error");
		return ret;
	}

	data->state = DMIC_STATE_CONFIGURED;
	return 0;
}
#endif /* DT_ANY_INST_ON_BUS_STATUS_OKAY(i2s) */
