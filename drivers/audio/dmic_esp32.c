/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DMIC adapter for the ESP32 I2S peripheral in PDM RX mode.
 *
 * The classic ESP32 captures a PDM microphone through its I2S peripheral, which
 * performs the PDM to PCM conversion in hardware. That peripheral is driven by
 * the i2s_esp32 driver. This adapter exposes the standard Zephyr DMIC API
 * (dmic_configure/trigger/read/get_caps) on top of the referenced I2S device,
 * so DMIC clients (for example the MediaPipe dmic_src element) can capture the
 * microphone without knowing it is backed by an I2S controller.
 *
 * The referenced I2S device must be built with CONFIG_I2S_ESP32_PDM_RX so that
 * its RX channel is configured for PDM.
 */

#define DT_DRV_COMPAT espressif_esp32_dmic

#include <errno.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dmic_esp32, CONFIG_AUDIO_DMIC_LOG_LEVEL);

struct dmic_esp32_config {
	const struct device *i2s_dev;
};

struct dmic_esp32_data {
	enum dmic_state state;
};

static int dmic_esp32_configure(const struct device *dev, struct dmic_cfg *config)
{
	const struct dmic_esp32_config *cfg = dev->config;
	struct dmic_esp32_data *data = dev->data;
	struct pcm_stream_cfg *stream;
	struct i2s_config i2s_cfg = {0};
	int ret;

	if ((config == NULL) || (config->streams == NULL)) {
		return -EINVAL;
	}

	if ((data->state == DMIC_STATE_ACTIVE) || (data->state == DMIC_STATE_PAUSED)) {
		return -EBUSY;
	}

	stream = &config->streams[0];

	/*
	 * Translate the DMIC stream request into an I2S RX configuration. The
	 * ESP32 is the clock controller for the microphone (options = 0), and
	 * the i2s_esp32 driver puts the RX channel into PDM mode.
	 */
	i2s_cfg.word_size = stream->pcm_width;
	i2s_cfg.channels = config->channel.req_num_chan;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options = 0;
	i2s_cfg.frame_clk_freq = stream->pcm_rate;
	i2s_cfg.mem_slab = stream->mem_slab;
	i2s_cfg.block_size = stream->block_size;
	i2s_cfg.timeout = SYS_FOREVER_MS;

	ret = i2s_configure(cfg->i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure backing I2S device: %d", ret);
		return ret;
	}

	/* Report back what was actually configured. */
	config->channel.act_num_streams = 1U;
	config->channel.act_num_chan = config->channel.req_num_chan;
	config->channel.act_chan_map_lo = config->channel.req_chan_map_lo;
	config->channel.act_chan_map_hi = config->channel.req_chan_map_hi;

	data->state = DMIC_STATE_CONFIGURED;

	return 0;
}

static int dmic_esp32_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	const struct dmic_esp32_config *cfg = dev->config;
	struct dmic_esp32_data *data = dev->data;
	int ret = 0;

	switch (cmd) {
	case DMIC_TRIGGER_START:
	case DMIC_TRIGGER_RELEASE:
		ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
		if (ret == 0) {
			data->state = DMIC_STATE_ACTIVE;
		}
		break;
	case DMIC_TRIGGER_STOP:
	case DMIC_TRIGGER_PAUSE:
		ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
		if (ret == 0) {
			data->state = (cmd == DMIC_TRIGGER_PAUSE) ? DMIC_STATE_PAUSED
								  : DMIC_STATE_CONFIGURED;
		}
		break;
	case DMIC_TRIGGER_RESET:
		ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
		data->state = DMIC_STATE_CONFIGURED;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static int dmic_esp32_read(const struct device *dev, uint8_t stream, void **buffer, size_t *size,
			   int32_t timeout)
{
	const struct dmic_esp32_config *cfg = dev->config;

	ARG_UNUSED(stream);
	/* The read timeout is taken from the I2S configuration, not per call. */
	ARG_UNUSED(timeout);

	return i2s_read(cfg->i2s_dev, buffer, size);
}

static int dmic_esp32_get_caps(const struct device *dev, struct audio_caps *caps)
{
	ARG_UNUSED(dev);

	memset(caps, 0, sizeof(struct audio_caps));

	caps->min_total_channels = 1;
	caps->max_total_channels = 2;
	caps->supported_sample_rates = AUDIO_SAMPLE_RATE_16000;
	caps->supported_bit_widths = AUDIO_BIT_WIDTH_16;
	caps->min_num_buffers = 2;
	caps->min_frame_interval = 1000;   /* 1ms minimum */
	caps->max_frame_interval = 100000; /* 100ms maximum */
	caps->interleaved = true;

	return 0;
}

static int dmic_esp32_init(const struct device *dev)
{
	const struct dmic_esp32_config *cfg = dev->config;
	struct dmic_esp32_data *data = dev->data;

	if (!device_is_ready(cfg->i2s_dev)) {
		LOG_ERR("Backing I2S device not ready");
		return -ENODEV;
	}

	data->state = DMIC_STATE_INITIALIZED;

	return 0;
}

static DEVICE_API(dmic, dmic_esp32_ops) = {
	.configure = dmic_esp32_configure,
	.trigger = dmic_esp32_trigger,
	.read = dmic_esp32_read,
	.get_caps = dmic_esp32_get_caps,
};

#define DMIC_ESP32_INIT(inst)                                                                      \
	static struct dmic_esp32_data dmic_esp32_data_##inst;                                       \
	static const struct dmic_esp32_config dmic_esp32_config_##inst = {                          \
		.i2s_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i2s)),                               \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, dmic_esp32_init, NULL, &dmic_esp32_data_##inst,                 \
			      &dmic_esp32_config_##inst, POST_KERNEL,                              \
			      CONFIG_AUDIO_DMIC_INIT_PRIORITY, &dmic_esp32_ops);

DT_INST_FOREACH_STATUS_OKAY(DMIC_ESP32_INIT)
