/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "i2s_mcux_sai_stream.h"

void i2s_mcux_sai_stream_purge(struct i2s_mcux_sai_stream *strm, bool in_drop, bool out_drop)
{
	struct i2s_mcux_sai_q_entry q_entry;

	if (in_drop) {
		while (k_msgq_get(&strm->in_queue, &q_entry, K_NO_WAIT) == 0) {
			k_mem_slab_free(strm->cfg.mem_slab, q_entry.mem_block);
		}
	}

	if (out_drop) {
		while (k_msgq_get(&strm->out_queue, &q_entry, K_NO_WAIT) == 0) {
			k_mem_slab_free(strm->cfg.mem_slab, q_entry.mem_block);
		}
	}
}

int i2s_mcux_sai_stream_dma_width(uint8_t word_size_bits, uint8_t *width)
{
	if (width == NULL) {
		return -EINVAL;
	}

	*width = word_size_bits / 8U;

	return 0;
}

int i2s_mcux_sai_stream_tx_reload(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				  uint32_t dest_address, uint8_t *blocks_queued)
{
	struct i2s_mcux_sai_q_entry q_entry = {0};
	unsigned int key;
	int ret = 0;

	*blocks_queued = 0;

	key = irq_lock();

	/* queue additional blocks to DMA if in_queue and DMA has free blocks */
	while (strm->free_tx_dma_blocks) {
		ret = k_msgq_get(&strm->in_queue, &q_entry, K_NO_WAIT);
		if (ret) {
			/* in_queue is empty, no more blocks to send to DMA */
			ret = 0;
			break;
		}

		ret = dma_reload(dma_dev, strm->dma.channel, (uint32_t)q_entry.mem_block,
				 dest_address, q_entry.size);
		if (ret != 0) {
			break;
		}

		(strm->free_tx_dma_blocks)--;

		ret = k_msgq_put(&strm->out_queue, &q_entry, K_NO_WAIT);
		if (ret != 0) {
			break;
		}

		(*blocks_queued)++;
	}

	irq_unlock(key);

	return ret;
}

int i2s_mcux_sai_stream_tx_start(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				 uint32_t dest_address)
{
	struct dma_block_config *blk_cfg = &strm->dma_block;
	struct i2s_mcux_sai_q_entry q_entry = {0};
	uint8_t blocks_queued;
	int ret;

	ret = k_msgq_get(&strm->in_queue, &q_entry, K_NO_WAIT);
	if (ret != 0) {
		return -EIO;
	}

	/* The driver keeps track of how many blocks the controller can take */
	strm->free_tx_dma_blocks = strm->max_dma_blocks;

	memset(blk_cfg, 0, sizeof(*blk_cfg));
	blk_cfg->dest_address = dest_address;
	blk_cfg->source_address = (uint32_t)q_entry.mem_block;
	blk_cfg->block_size = q_entry.size;
	if (!strm->dma.request_channel) {
		blk_cfg->dest_scatter_en = 1U;
	}

	strm->dma_cfg.block_count = 1;
	strm->dma_cfg.head_block = blk_cfg;

	(strm->free_tx_dma_blocks)--;
	dma_config(dma_dev, strm->dma.channel, &strm->dma_cfg);

	ret = k_msgq_put(&strm->out_queue, &q_entry, K_NO_WAIT);
	if (ret != 0) {
		return ret;
	}

	ret = i2s_mcux_sai_stream_tx_reload(strm, dma_dev, dest_address, &blocks_queued);
	if (ret != 0) {
		return ret;
	}

	return dma_start(dma_dev, strm->dma.channel);
}

int i2s_mcux_sai_stream_rx_start(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				 uint32_t source_address)
{
	struct dma_block_config *blk_cfg = &strm->dma_block;
	struct i2s_mcux_sai_q_entry q_entry = {0};
	int ret;

	/*
	 * Need at least I2S_MCUX_SAI_RX_PREP_BLOCKS buffers on the RX memory
	 * slab for reliable DMA reception.
	 */
	if (k_mem_slab_num_free_get(strm->cfg.mem_slab) < I2S_MCUX_SAI_RX_PREP_BLOCKS) {
		return -EINVAL;
	}

	ret = k_mem_slab_alloc(strm->cfg.mem_slab, &q_entry.mem_block, K_NO_WAIT);
	if (ret != 0) {
		return ret;
	}
	q_entry.size = strm->cfg.block_size;

	memset(blk_cfg, 0, sizeof(*blk_cfg));
	blk_cfg->dest_address = (uint32_t)q_entry.mem_block;
	blk_cfg->source_address = source_address;
	blk_cfg->block_size = q_entry.size;
	if (!strm->dma.request_channel) {
		blk_cfg->source_gather_en = 1U;
	}

	strm->dma_cfg.block_count = 1;
	strm->dma_cfg.head_block = blk_cfg;

	dma_config(dma_dev, strm->dma.channel, &strm->dma_cfg);

	ret = k_msgq_put(&strm->in_queue, &q_entry, K_NO_WAIT);
	if (ret != 0) {
		return ret;
	}

	for (int i = 0; i < I2S_MCUX_SAI_RX_PREP_BLOCKS - 1; i++) {
		ret = k_mem_slab_alloc(strm->cfg.mem_slab, &q_entry.mem_block, K_NO_WAIT);
		if (ret != 0) {
			return ret;
		}
		q_entry.size = blk_cfg->block_size;

		ret = dma_reload(dma_dev, strm->dma.channel, source_address,
				 (uint32_t)q_entry.mem_block, q_entry.size);
		if (ret != 0) {
			return ret;
		}

		ret = k_msgq_put(&strm->in_queue, &q_entry, K_NO_WAIT);
		if (ret != 0) {
			return ret;
		}
	}

	return dma_start(dma_dev, strm->dma.channel);
}

enum i2s_mcux_sai_stream_action
i2s_mcux_sai_stream_tx_complete(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				uint32_t dest_address, int status)
{
	struct i2s_mcux_sai_q_entry q_entry = {0};
	uint8_t blocks_queued;
	int ret;

	ARG_UNUSED(status);

	ret = k_msgq_get(&strm->out_queue, &q_entry, K_NO_WAIT);
	if (ret == 0) {
		/* transmission complete, the buffer is the driver's again */
		k_mem_slab_free(strm->cfg.mem_slab, q_entry.mem_block);
		(strm->free_tx_dma_blocks)++;
	}

	if (strm->free_tx_dma_blocks > strm->max_dma_blocks) {
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	/* Received a STOP trigger, terminate TX immediately */
	if (strm->last_block) {
		strm->state = I2S_STATE_READY;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	if (ret != 0) {
		/* out_queue was empty and this was not the last block */
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	if (strm->state != I2S_STATE_RUNNING && strm->state != I2S_STATE_STOPPING) {
		return I2S_MCUX_SAI_STREAM_STOP_DROP;
	}

	ret = i2s_mcux_sai_stream_tx_reload(strm, dma_dev, dest_address, &blocks_queued);
	if (ret != 0) {
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	if (blocks_queued || (strm->free_tx_dma_blocks < strm->max_dma_blocks)) {
		return I2S_MCUX_SAI_STREAM_RUN;
	}

	/* all DMA blocks are free but no blocks were queued */
	if (strm->state == I2S_STATE_STOPPING) {
		strm->state = I2S_STATE_READY;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	/* Nothing left to transmit: gate the bit clock so that the receiver
	 * side does not sample dummy bits.
	 */
	strm->state = I2S_STATE_READY;

	return I2S_MCUX_SAI_STREAM_PAUSE;
}

enum i2s_mcux_sai_stream_action
i2s_mcux_sai_stream_rx_complete(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				uint32_t source_address, int status)
{
	struct i2s_mcux_sai_q_entry q_entry;
	int ret;

	ARG_UNUSED(status);

	if (strm->state == I2S_STATE_ERROR) {
		return I2S_MCUX_SAI_STREAM_STOP_DROP;
	}

	if (strm->state != I2S_STATE_STOPPING && strm->state != I2S_STATE_RUNNING) {
		return I2S_MCUX_SAI_STREAM_RUN;
	}

	ret = k_msgq_get(&strm->in_queue, &q_entry, K_NO_WAIT);
	__ASSERT_NO_MSG(ret == 0);

	ret = k_msgq_put(&strm->out_queue, &q_entry, K_NO_WAIT);
	if (ret != 0) {
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	if (strm->state == I2S_STATE_STOPPING) {
		/* Received a STOP/DRAIN trigger */
		strm->state = I2S_STATE_READY;
		return I2S_MCUX_SAI_STREAM_STOP_DRAIN;
	}

	/* allocate a new buffer for the next audio frame */
	ret = k_mem_slab_alloc(strm->cfg.mem_slab, &q_entry.mem_block, K_NO_WAIT);
	if (ret != 0) {
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}
	q_entry.size = strm->cfg.block_size;

	ret = dma_reload(dma_dev, strm->dma.channel, source_address, (uint32_t)q_entry.mem_block,
			 q_entry.size);
	if (ret != 0) {
		strm->state = I2S_STATE_ERROR;
		return I2S_MCUX_SAI_STREAM_STOP;
	}

	ret = k_msgq_put(&strm->in_queue, &q_entry, K_NO_WAIT);
	if (ret != 0) {
		return I2S_MCUX_SAI_STREAM_RUN;
	}

	return I2S_MCUX_SAI_STREAM_RUN;
}
