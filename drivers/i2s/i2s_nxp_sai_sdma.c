/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S driver for NXP SAI instances served by an SDMA engine, as found on the
 * Cortex-M cores of the i.MX8M family (for example SAI3 driven by SDMA3 inside
 * the i.MX8MP AUDIOMIX domain).
 *
 * SAI is programmed through the register-level fsl_sai HAL, while the data
 * movement goes through the Zephyr DMA API. The SDMA instance therefore stays
 * owned by its own DMA controller driver and remains shareable with the other
 * peripherals wired to it, and the channels come from the standard
 * "dmas"/"dma-names" properties rather than from anything SAI specific.
 */

#define DT_DRV_COMPAT nxp_sai_sdma

#include <zephyr/audio/audio_caps.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <string.h>

#include <fsl_sai.h>

LOG_MODULE_REGISTER(i2s_nxp_sai_sdma, CONFIG_I2S_LOG_LEVEL);

/*
 * Descriptors the caller has not filled yet transfer from here, so the engine
 * always has something valid to move and never stops mid-stream waiting for a
 * buffer. On capture it is a scratch sink whose contents are discarded.
 *
 * Sized in bytes rather than derived from the block size, which is only known
 * once a format is negotiated. Longer costs memory and delays the point at
 * which a late block can take over; shorter makes the ring drain unevenly,
 * because a silence descriptor then completes several times per real block.
 * Shared by every instance, and never handed to the pipeline.
 */
static uint32_t sai_sdma_silence[CONFIG_I2S_NXP_SAI_SDMA_SILENCE_SIZE / sizeof(uint32_t)]
	__aligned(32);

#define SAI_SDMA_QUEUE_DEPTH CONFIG_I2S_NXP_SAI_SDMA_QUEUE_DEPTH

/*
 * Every queued block occupies a buffer descriptor until the DMA completes it,
 * so queueing more than the channel has descriptors for would overwrite one
 * that is still in flight.
 */
BUILD_ASSERT(SAI_SDMA_QUEUE_DEPTH <= CONFIG_DMA_NXP_SDMA_BD_COUNT,
	     "I2S queue depth exceeds the SDMA buffer descriptors per channel");

struct sai_sdma_block {
	void *buffer;
	size_t size;
};

/*
 * One direction's worth of state. Blocks move in_queue -> hardware ->
 * out_queue: in_queue holds what the DMA has been given and not yet completed,
 * out_queue holds what is finished and waiting to be collected by the
 * application on capture.
 */
struct sai_sdma_stream {
	enum i2s_state state;
	struct i2s_config cfg;

	const struct device *dma_dev;
	uint32_t dma_request; /* SDMA request line this direction is wired to */
	uint32_t dma_periph;  /* SDMA peripheral type (sdma_peripheral_t) */
	int dma_channel;      /* allocated at configure() time, -1 when unheld */
	struct dma_config dma_cfg;
	struct dma_block_config dma_blocks[SAI_SDMA_QUEUE_DEPTH];
	bool dma_started;

	struct k_msgq in_queue;
	struct k_msgq out_queue;
	char in_buf[SAI_SDMA_QUEUE_DEPTH * sizeof(struct sai_sdma_block)];
	char out_buf[SAI_SDMA_QUEUE_DEPTH * sizeof(struct sai_sdma_block)];

	uint32_t blocks_done;
};

struct sai_sdma_cfg {
	I2S_Type *base;
	const struct device *ccm_dev;
	clock_control_subsys_t clk_sub;
	const struct pinctrl_dev_config *pincfg;
	void (*irq_config)(void);
	uint32_t irq;
	bool mclk_output;
	bool rx_sync;
};

struct sai_sdma_data {
	struct sai_sdma_stream tx;
	struct sai_sdma_stream rx;
	struct k_spinlock lock;
};

static struct sai_sdma_stream *sai_sdma_get_stream(const struct device *dev, enum i2s_dir dir)
{
	struct sai_sdma_data *data = dev->data;

	return dir == I2S_DIR_TX ? &data->tx : &data->rx;
}

static uint32_t sai_sdma_fifo_addr(const struct device *dev, enum i2s_dir dir)
{
	const struct sai_sdma_cfg *cfg = dev->config;

	/* data line 0 only; the FIFO register address must not be incremented */
	return dir == I2S_DIR_TX ? (uint32_t)&cfg->base->TDR[0] : (uint32_t)&cfg->base->RDR[0];
}

static void sai_sdma_free_all(struct sai_sdma_stream *stream)
{
	struct sai_sdma_block blk;

	if (stream->cfg.mem_slab == NULL) {
		return;
	}

	while (k_msgq_get(&stream->in_queue, &blk, K_NO_WAIT) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
	}

	while (k_msgq_get(&stream->out_queue, &blk, K_NO_WAIT) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
	}
}

static void sai_sdma_stop_hw(const struct device *dev, enum i2s_dir dir)
{
	const struct sai_sdma_cfg *cfg = dev->config;
	struct sai_sdma_stream *stream = sai_sdma_get_stream(dev, dir);

	if (stream->dma_started) {
		dma_stop(stream->dma_dev, stream->dma_channel);
		stream->dma_started = false;
	}

	if (dir == I2S_DIR_TX) {
		SAI_TxEnableDMA(cfg->base, kSAI_FIFORequestDMAEnable, false);
		SAI_TxEnableInterrupts(cfg->base, 0U);
		SAI_TxEnable(cfg->base, false);
		SAI_TxSoftwareReset(cfg->base, kSAI_ResetTypeFIFO);
	} else {
		SAI_RxEnableDMA(cfg->base, kSAI_FIFORequestDMAEnable, false);
		SAI_RxEnableInterrupts(cfg->base, 0U);
		SAI_RxEnable(cfg->base, false);
		SAI_RxSoftwareReset(cfg->base, kSAI_ResetTypeFIFO);
	}
}

/*
 * Hand one block to the DMA engine. The first block of a stream goes in through
 * dma_config()/dma_start(); every later one through dma_reload(), which is what
 * tells the DMA driver a fresh buffer address has arrived.
 */
static int sai_sdma_submit(const struct device *dev, enum i2s_dir dir,
			   const struct sai_sdma_block *blk, k_timeout_t timeout)
{
	struct sai_sdma_stream *stream = sai_sdma_get_stream(dev, dir);
	uint32_t fifo = sai_sdma_fifo_addr(dev, dir);
	uint32_t src, dst;
	int ret;

	if (dir == I2S_DIR_TX) {
		src = (uint32_t)blk->buffer;
		dst = fifo;
	} else {
		src = fifo;
		dst = (uint32_t)blk->buffer;
	}

	if (!stream->dma_started) {
		/*
		 * Arm every descriptor so the engine can run ahead instead of
		 * stopping each time it reaches an unfilled one. Only the first
		 * carries this block; the rest transfer silence until their own
		 * buffers are queued, which keeps the ring valid without
		 * resending audio the pipeline has taken back.
		 */
		for (int i = 0; i < SAI_SDMA_QUEUE_DEPTH; i++) {
			bool first = i == 0;
			uint32_t addr = first ? (uint32_t)blk->buffer
					      : (uint32_t)sai_sdma_silence;

			stream->dma_blocks[i].source_address =
				dir == I2S_DIR_TX ? addr : fifo;
			stream->dma_blocks[i].dest_address =
				dir == I2S_DIR_TX ? fifo : addr;
			stream->dma_blocks[i].block_size =
				first ? blk->size : sizeof(sai_sdma_silence);
			stream->dma_blocks[i].next_block =
				(i + 1 < SAI_SDMA_QUEUE_DEPTH) ? &stream->dma_blocks[i + 1] : NULL;
		}

		ret = dma_config(stream->dma_dev, stream->dma_channel, &stream->dma_cfg);
		if (ret < 0) {
			LOG_ERR("dma_config failed: %d", ret);
			return ret;
		}

		ret = dma_start(stream->dma_dev, stream->dma_channel);
		if (ret < 0) {
			LOG_ERR("dma_start failed: %d", ret);
			return ret;
		}

		stream->dma_started = true;
	} else {
		ret = dma_reload(stream->dma_dev, stream->dma_channel, src, dst, blk->size);
		if (ret < 0) {
			LOG_ERR("dma_reload failed: %d", ret);
			return ret;
		}
	}

	return k_msgq_put(&stream->in_queue, blk, timeout);
}

/* keep the receiver supplied with empty blocks for as long as the slab allows */
static void sai_sdma_rx_refill(const struct device *dev)
{
	struct sai_sdma_stream *stream = sai_sdma_get_stream(dev, I2S_DIR_RX);
	struct sai_sdma_block blk;

	while (k_msgq_num_used_get(&stream->in_queue) < SAI_SDMA_QUEUE_DEPTH) {
		if (k_mem_slab_alloc(stream->cfg.mem_slab, &blk.buffer, K_NO_WAIT) != 0) {
			/* capture pauses until the application returns a block */
			return;
		}

		blk.size = stream->cfg.block_size;

		if (sai_sdma_submit(dev, I2S_DIR_RX, &blk, K_NO_WAIT) < 0) {
			k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
			stream->state = I2S_STATE_ERROR;
			return;
		}
	}
}

static void sai_sdma_tx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
				 int status)
{
	const struct device *dev = arg;
	struct sai_sdma_data *data = dev->data;
	struct sai_sdma_stream *stream = &data->tx;
	struct sai_sdma_block blk;
	k_spinlock_key_t key;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	key = k_spin_lock(&data->lock);

	if (status < 0) {
		LOG_ERR("TX DMA error: %d", status);
		stream->state = I2S_STATE_ERROR;
		goto out;
	}

	/*
	 * Descriptors are filled and consumed in the same order, so reaching one
	 * that still holds silence means every queued block sits behind it and
	 * has already completed. A completion with nothing queued is therefore a
	 * silence descriptor, and there is nothing of the caller's to release.
	 */
	if (k_msgq_get(&stream->in_queue, &blk, K_NO_WAIT) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
		stream->blocks_done++;
	}

	switch (stream->state) {
	case I2S_STATE_RUNNING:
		if (k_msgq_num_used_get(&stream->in_queue) == 0) {
			/* nothing left in flight: the application fell behind */
			LOG_ERR("TX underrun after %u blocks", stream->blocks_done);
			stream->state = I2S_STATE_ERROR;
		}
		break;
	case I2S_STATE_STOPPING:
		/*
		 * Reached from both STOP and DRAIN. They differ in whether the
		 * blocks still queued were dropped at trigger time; by the point
		 * the last one in flight completes, the handling is the same.
		 */
		if (k_msgq_num_used_get(&stream->in_queue) == 0) {
			sai_sdma_stop_hw(dev, I2S_DIR_TX);
			sai_sdma_free_all(stream);
			stream->state = I2S_STATE_READY;
		}
		break;
	default:
		break;
	}

out:
	k_spin_unlock(&data->lock, key);
}

static void sai_sdma_rx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
				 int status)
{
	const struct device *dev = arg;
	struct sai_sdma_data *data = dev->data;
	struct sai_sdma_stream *stream = &data->rx;
	struct sai_sdma_block blk;
	k_spinlock_key_t key;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	key = k_spin_lock(&data->lock);

	if (status < 0) {
		LOG_ERR("RX DMA error: %d", status);
		stream->state = I2S_STATE_ERROR;
		goto out;
	}

	if (k_msgq_get(&stream->in_queue, &blk, K_NO_WAIT) == 0) {
		if (k_msgq_put(&stream->out_queue, &blk, K_NO_WAIT) != 0) {
			/* the reader is behind: drop this block rather than stall */
			k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
		}
		stream->blocks_done++;
	}

	if (stream->state == I2S_STATE_RUNNING) {
		sai_sdma_rx_refill(dev);
	} else if (stream->state == I2S_STATE_STOPPING) {
		sai_sdma_stop_hw(dev, I2S_DIR_RX);
		stream->state = I2S_STATE_READY;
	}

out:
	k_spin_unlock(&data->lock, key);
}

static void sai_sdma_isr(const void *arg)
{
	const struct device *dev = arg;
	const struct sai_sdma_cfg *cfg = dev->config;
	struct sai_sdma_data *data = dev->data;

	/*
	 * A FIFO error latches and holds the transceiver stopped until it is
	 * acknowledged, so clear it and mark the stream. Without this the
	 * application sees an indefinite stall rather than an error it can
	 * recover from with PREPARE.
	 */
	if ((SAI_TxGetStatusFlag(cfg->base) & kSAI_FIFOErrorFlag) != 0U) {
		SAI_TxClearStatusFlags(cfg->base, kSAI_FIFOErrorFlag);

		if (data->tx.state == I2S_STATE_RUNNING) {
			LOG_ERR("TX FIFO underrun");
			data->tx.state = I2S_STATE_ERROR;
		}
	}

	if ((SAI_RxGetStatusFlag(cfg->base) & kSAI_FIFOErrorFlag) != 0U) {
		SAI_RxClearStatusFlags(cfg->base, kSAI_FIFOErrorFlag);

		if (data->rx.state == I2S_STATE_RUNNING) {
			LOG_ERR("RX FIFO overrun");
			data->rx.state = I2S_STATE_ERROR;
		}
	}
}

static int sai_sdma_configure(const struct device *dev, enum i2s_dir dir,
			      const struct i2s_config *i2s_cfg)
{
	const struct sai_sdma_cfg *cfg = dev->config;
	struct sai_sdma_stream *stream;
	sai_transceiver_t transceiver;
	sai_word_width_t word_width;
	sai_mono_stereo_t mono_stereo;
	uint32_t mclk_rate;
	uint8_t word_bytes;
	int ret;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		LOG_ERR("unsupported direction %d", dir);
		return -ENOSYS;
	}

	stream = sai_sdma_get_stream(dev, dir);

	if (stream->state == I2S_STATE_RUNNING || stream->state == I2S_STATE_STOPPING) {
		return -EBUSY;
	}

	if ((i2s_cfg->format & I2S_FMT_DATA_FORMAT_MASK) != I2S_FMT_DATA_FORMAT_I2S) {
		LOG_ERR("only classic I2S data format is supported");
		return -EINVAL;
	}

	switch (i2s_cfg->word_size) {
	case 16U:
		word_width = kSAI_WordWidth16bits;
		break;
	case 24U:
		word_width = kSAI_WordWidth24bits;
		break;
	case 32U:
		word_width = kSAI_WordWidth32bits;
		break;
	default:
		LOG_ERR("unsupported word size %u", i2s_cfg->word_size);
		return -EINVAL;
	}

	if (i2s_cfg->channels == 1U) {
		mono_stereo = kSAI_MonoLeft;
	} else if (i2s_cfg->channels == 2U) {
		mono_stereo = kSAI_Stereo;
	} else {
		LOG_ERR("unsupported channel count %u", i2s_cfg->channels);
		return -EINVAL;
	}

	if (i2s_cfg->mem_slab == NULL || i2s_cfg->block_size == 0U) {
		return -EINVAL;
	}

	ret = clock_control_get_rate(cfg->ccm_dev, cfg->clk_sub, &mclk_rate);
	if (ret < 0) {
		LOG_ERR("failed to query SAI clock rate: %d", ret);
		return ret;
	}

	/*
	 * The request line is what ties this direction to its peripheral, and
	 * the DMA driver latches it when the channel is handed out, so a channel
	 * has to be held before anything is configured on it.
	 */
	if (stream->dma_channel < 0) {
		ret = dma_request_channel(stream->dma_dev, &stream->dma_request);
		if (ret < 0) {
			LOG_ERR("no free DMA channel for request %u: %d", stream->dma_request,
				ret);
			return ret;
		}

		stream->dma_channel = ret;
	}

	SAI_GetClassicI2SConfig(&transceiver, word_width, mono_stereo, kSAI_Channel0Mask);

	if ((i2s_cfg->options & I2S_OPT_BIT_CLK_TARGET) != 0U) {
		transceiver.masterSlave = kSAI_Slave;
	} else {
		transceiver.masterSlave = kSAI_Master;
	}

	if (dir == I2S_DIR_RX && cfg->rx_sync) {
		/*
		 * Boards that wire a single bit/frame clock pair to the codec
		 * leave the receiver without clocks of its own, so it samples on
		 * the transmitter's. Capture then produces nothing until TX runs,
		 * which callers driving both directions have to account for.
		 */
		transceiver.syncMode = kSAI_ModeSync;
	}

	if (dir == I2S_DIR_TX) {
		SAI_TxSetConfig(cfg->base, &transceiver);
		SAI_TxSetBitClockRate(cfg->base, mclk_rate, i2s_cfg->frame_clk_freq,
				      i2s_cfg->word_size, i2s_cfg->channels);
	} else {
		SAI_RxSetConfig(cfg->base, &transceiver);
		SAI_RxSetBitClockRate(cfg->base, mclk_rate, i2s_cfg->frame_clk_freq,
				      i2s_cfg->word_size, i2s_cfg->channels);
	}

	if (cfg->mclk_output) {
		sai_master_clock_t mclk_config = {
			.mclkOutputEnable = true,
			.mclkHz = mclk_rate,
			.mclkSourceClkHz = mclk_rate,
		};

		SAI_SetMasterClockConfig(cfg->base, &mclk_config);
	}

	sai_sdma_free_all(stream);

	stream->cfg = *i2s_cfg;
	word_bytes = i2s_cfg->word_size / 8U;


	stream->dma_cfg.dma_slot = stream->dma_periph;
	stream->dma_cfg.channel_direction =
		dir == I2S_DIR_TX ? MEMORY_TO_PERIPHERAL : PERIPHERAL_TO_MEMORY;
	stream->dma_cfg.source_data_size = word_bytes;
	stream->dma_cfg.dest_data_size = word_bytes;
	/*
	 * The burst length is the peripheral's FIFO watermark: the DMA has to
	 * request service at the same depth the SAI signals at, or it will run
	 * ahead of the FIFO and fault.
	 */
	stream->dma_cfg.source_burst_length = word_bytes;
	stream->dma_cfg.dest_burst_length = word_bytes;
	stream->dma_cfg.block_count = SAI_SDMA_QUEUE_DEPTH;
	stream->dma_cfg.head_block = &stream->dma_blocks[0];
	stream->dma_cfg.user_data = (void *)dev;
	stream->dma_cfg.dma_callback =
		dir == I2S_DIR_TX ? sai_sdma_tx_callback : sai_sdma_rx_callback;
	stream->dma_cfg.complete_callback_en = 1U;

	stream->blocks_done = 0U;
	stream->dma_started = false;
	stream->state = I2S_STATE_READY;

	LOG_DBG("%s configured: rate %u ws %u ch %u blksz %u", dir == I2S_DIR_TX ? "TX" : "RX",
		i2s_cfg->frame_clk_freq, i2s_cfg->word_size, i2s_cfg->channels,
		i2s_cfg->block_size);

	return 0;
}

static const struct i2s_config *sai_sdma_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct sai_sdma_stream *stream;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return NULL;
	}

	stream = sai_sdma_get_stream(dev, dir);

	return stream->state == I2S_STATE_NOT_READY ? NULL : &stream->cfg;
}

static int sai_sdma_write(const struct device *dev, void *mem_block, size_t size)
{
	struct sai_sdma_data *data = dev->data;
	struct sai_sdma_stream *stream = &data->tx;
	struct sai_sdma_block blk = {
		.buffer = mem_block,
		.size = size,
	};
	k_spinlock_key_t key;
	k_timepoint_t deadline;
	int ret;

	if (stream->state != I2S_STATE_READY && stream->state != I2S_STATE_RUNNING) {
		return -EIO;
	}

	/*
	 * A full queue is back pressure, not a failure: the API expects the
	 * caller to be held here until a block frees up or the configured
	 * timeout expires. Returning an error instead makes callers that treat
	 * any write failure as a latched fault reset their own state while the
	 * stream is still running, leaving the two out of step.
	 */
	deadline = sys_timepoint_calc(SYS_TIMEOUT_MS(stream->cfg.timeout));

	while (k_msgq_num_free_get(&stream->in_queue) == 0) {
		if (sys_timepoint_expired(deadline)) {
			return -EAGAIN;
		}

		if (k_is_in_isr()) {
			return -EAGAIN;
		}

		k_sleep(K_MSEC(1));
	}

	key = k_spin_lock(&data->lock);
	ret = sai_sdma_submit(dev, I2S_DIR_TX, &blk, K_NO_WAIT);
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int sai_sdma_read(const struct device *dev, void **mem_block, size_t *size)
{
	struct sai_sdma_data *data = dev->data;
	struct sai_sdma_stream *stream = &data->rx;
	struct sai_sdma_block blk;
	k_spinlock_key_t key;
	int ret;

	if (stream->state == I2S_STATE_NOT_READY || stream->state == I2S_STATE_ERROR) {
		return -EIO;
	}

	ret = k_msgq_get(&stream->out_queue, &blk, SYS_TIMEOUT_MS(stream->cfg.timeout));
	if (ret < 0) {
		return -EAGAIN;
	}

	*mem_block = blk.buffer;
	*size = blk.size;

	/* the application now holds a slab block; refill once it frees one */
	key = k_spin_lock(&data->lock);
	if (stream->state == I2S_STATE_RUNNING) {
		sai_sdma_rx_refill(dev);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int sai_sdma_trigger_start(const struct device *dev, enum i2s_dir dir)
{
	const struct sai_sdma_cfg *cfg = dev->config;
	struct sai_sdma_stream *stream = sai_sdma_get_stream(dev, dir);


	if (stream->state != I2S_STATE_READY) {
		LOG_ERR("START in invalid state %d", stream->state);
		return -EIO;
	}

	stream->state = I2S_STATE_RUNNING;

	if (dir == I2S_DIR_RX) {
		sai_sdma_rx_refill(dev);

		if (k_msgq_num_used_get(&stream->in_queue) == 0) {
			stream->state = I2S_STATE_READY;
			LOG_ERR("no buffers available to start capture");
			return -ENOMEM;
		}

		SAI_RxEnableDMA(cfg->base, kSAI_FIFORequestDMAEnable, true);
		SAI_RxEnableInterrupts(cfg->base, kSAI_FIFOErrorInterruptEnable);
		SAI_RxEnable(cfg->base, true);
	} else {
		if (k_msgq_num_used_get(&stream->in_queue) == 0) {
			stream->state = I2S_STATE_READY;
			LOG_ERR("no data queued to start playback");
			return -EIO;
		}

		SAI_TxEnableDMA(cfg->base, kSAI_FIFORequestDMAEnable, true);
		SAI_TxEnableInterrupts(cfg->base, kSAI_FIFOErrorInterruptEnable);
		SAI_TxEnable(cfg->base, true);
	}

	return 0;
}

static int sai_sdma_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct sai_sdma_data *data = dev->data;
	struct sai_sdma_stream *stream;
	k_spinlock_key_t key;
	int ret = 0;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return -ENOSYS;
	}

	stream = sai_sdma_get_stream(dev, dir);
	key = k_spin_lock(&data->lock);

	switch (cmd) {
	case I2S_TRIGGER_START:
		ret = sai_sdma_trigger_start(dev, dir);
		break;

	case I2S_TRIGGER_STOP:
	case I2S_TRIGGER_DRAIN:
		if (stream->state != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}

		/*
		 * Both let the block already in flight finish, since cutting a
		 * transfer mid-block leaves the FIFO in an error state. They
		 * differ only in what happens to the blocks behind it: DRAIN
		 * plays them out, STOP discards them.
		 */
		if (cmd == I2S_TRIGGER_STOP && dir == I2S_DIR_TX) {
			struct sai_sdma_block blk;

			while (k_msgq_num_used_get(&stream->in_queue) > 1 &&
			       k_msgq_get(&stream->in_queue, &blk, K_NO_WAIT) == 0) {
				k_mem_slab_free(stream->cfg.mem_slab, blk.buffer);
			}
		}

		stream->state = I2S_STATE_STOPPING;
		break;

	case I2S_TRIGGER_DROP:
		if (stream->state == I2S_STATE_NOT_READY) {
			ret = -EIO;
			break;
		}

		sai_sdma_stop_hw(dev, dir);
		sai_sdma_free_all(stream);
		stream->state = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_PREPARE:
		if (stream->state != I2S_STATE_ERROR) {
			ret = -EIO;
			break;
		}

		sai_sdma_stop_hw(dev, dir);
		sai_sdma_free_all(stream);
		stream->state = I2S_STATE_READY;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	k_spin_unlock(&data->lock, key);

	return ret;
}

static int sai_sdma_init(const struct device *dev)
{
	const struct sai_sdma_cfg *cfg = dev->config;
	struct sai_sdma_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->ccm_dev)) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(data->tx.dma_dev) || !device_is_ready(data->rx.dma_dev)) {
		LOG_ERR("DMA device not ready");
		return -ENODEV;
	}

	ret = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0 && ret != -ENOENT) {
		return ret;
	}

	ret = clock_control_on(cfg->ccm_dev, cfg->clk_sub);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("failed to enable SAI clock: %d", ret);
		return ret;
	}

	SAI_Init(cfg->base);

	k_msgq_init(&data->tx.in_queue, data->tx.in_buf, sizeof(struct sai_sdma_block),
		    SAI_SDMA_QUEUE_DEPTH);
	k_msgq_init(&data->tx.out_queue, data->tx.out_buf, sizeof(struct sai_sdma_block),
		    SAI_SDMA_QUEUE_DEPTH);
	k_msgq_init(&data->rx.in_queue, data->rx.in_buf, sizeof(struct sai_sdma_block),
		    SAI_SDMA_QUEUE_DEPTH);
	k_msgq_init(&data->rx.out_queue, data->rx.out_buf, sizeof(struct sai_sdma_block),
		    SAI_SDMA_QUEUE_DEPTH);

	data->tx.state = I2S_STATE_NOT_READY;
	data->rx.state = I2S_STATE_NOT_READY;
	data->tx.dma_channel = -1;
	data->rx.dma_channel = -1;

	cfg->irq_config();
	irq_enable(cfg->irq);

	return 0;
}

static int sai_sdma_get_caps(const struct device *dev, struct audio_caps *caps, enum i2s_dir dir)
{
	ARG_UNUSED(dev);

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return -ENOSYS;
	}

	memset(caps, 0, sizeof(*caps));
	caps->min_total_channels = 1U;
	caps->max_total_channels = 2U;
	/*
	 * Only the 48 kHz family: the SAI root clock is derived from AUDIO PLL1
	 * (393.216 MHz), so the 44.1 kHz rates are not reachable.
	 */
	caps->supported_sample_rates = AUDIO_SAMPLE_RATE_8000 | AUDIO_SAMPLE_RATE_16000 |
				       AUDIO_SAMPLE_RATE_32000 | AUDIO_SAMPLE_RATE_48000;
	caps->supported_bit_widths =
		AUDIO_BIT_WIDTH_16 | AUDIO_BIT_WIDTH_24 | AUDIO_BIT_WIDTH_32;
	/*
	 * Each direction keeps up to SAI_SDMA_QUEUE_DEPTH blocks in flight, and
	 * the producer needs headroom on top of that. The margin also covers the
	 * silence prime a sink issues when capture is clock-slaved to playback
	 * and both directions share one device.
	 */
	caps->min_num_buffers = SAI_SDMA_QUEUE_DEPTH + 8U;
	caps->min_frame_interval = 1000U;
	caps->max_frame_interval = 100000U;
	caps->interleaved = true;

	return 0;
}

static DEVICE_API(i2s, sai_sdma_api) = {
	.configure = sai_sdma_configure,
	.config_get = sai_sdma_config_get,
	.get_caps = sai_sdma_get_caps,
	.read = sai_sdma_read,
	.write = sai_sdma_write,
	.trigger = sai_sdma_trigger,
};

/*
 * The "channel" cell carries the SDMA request line the peripheral is wired to
 * and the "mux" cell the SDMA peripheral type, matching how the SAI nodes
 * already in tree are written.
 */
#define SAI_SDMA_STREAM_INIT(inst, dir)                                                            \
	{                                                                                          \
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, dir)),                    \
		.dma_request = DT_INST_DMAS_CELL_BY_NAME(inst, dir, channel),                      \
		.dma_periph = DT_INST_DMAS_CELL_BY_NAME(inst, dir, mux),                           \
		.dma_channel = -1,                                                                 \
	}

#define I2S_NXP_SAI_SDMA_INIT(inst)                                                                \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
                                                                                                   \
	static void sai_sdma_irq_config_##inst(void)                                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), sai_sdma_isr,         \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
	}                                                                                          \
                                                                                                   \
	static const struct sai_sdma_cfg sai_sdma_cfg_##inst = {                                   \
		.base = (I2S_Type *)DT_INST_REG_ADDR(inst),                                        \
		.ccm_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                               \
		.clk_sub = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(inst, name),                \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                    \
		.irq_config = sai_sdma_irq_config_##inst,                                          \
		.irq = DT_INST_IRQN(inst),                                                         \
		.mclk_output = DT_INST_PROP(inst, mclk_output),                                    \
		.rx_sync = DT_INST_PROP(inst, rx_sync_mode),                                       \
	};                                                                                         \
                                                                                                   \
	static struct sai_sdma_data sai_sdma_data_##inst = {                                       \
		.tx = SAI_SDMA_STREAM_INIT(inst, tx),                                              \
		.rx = SAI_SDMA_STREAM_INIT(inst, rx),                                              \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &sai_sdma_init, NULL, &sai_sdma_data_##inst,                   \
			      &sai_sdma_cfg_##inst, POST_KERNEL, CONFIG_I2S_INIT_PRIORITY,         \
			      &sai_sdma_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_NXP_SAI_SDMA_INIT)
