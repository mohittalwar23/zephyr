/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_STREAM_H_
#define ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_STREAM_H_

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#include "i2s_mcux_sai_dma.h"

#define I2S_MCUX_SAI_RX_PREP_BLOCKS 3

struct i2s_mcux_sai_q_entry {
	void *mem_block;
	size_t size;
};

struct i2s_mcux_sai_tx_fifo_config {
	uint32_t watermark;
	uint32_t burst_length;
};

/*
 * The SAI driver relies on the DMA controller managing a circular queue of
 * blocks. eDMA expresses this with gather/scatter; SDMA uses a driver-specific
 * append mode in dma_slot. Calling dma_reload() adds a new block to the
 * configured channel.
 *
 * in_queue and out_queue are used as follows
 *   transmit stream:
 *   application provided buffer is queued to in_queue until loaded to DMA.
 *   when DMA channel is idle, buffer is retrieved from in_queue and loaded
 *   to DMA and queued to out_queue. when DMA completes, buffer is retrieved
 *   from out_queue and freed.
 *
 *   receive stream:
 *   driver allocates buffer from slab and loads DMA buffer is queued to
 *   in_queue when DMA completes, buffer is retrieved from in_queue
 *   and queued to out_queue when application reads, buffer is read
 *   (may optionally block) from out_queue and presented to application.
 */
struct i2s_mcux_sai_stream {
	enum i2s_state state;
	struct i2s_mcux_sai_dma_channel dma;
	uint8_t max_dma_blocks;
	uint32_t start_channel;
	struct i2s_config cfg;
	struct dma_config dma_cfg;
	struct dma_block_config dma_block;
	uint8_t free_tx_dma_blocks;
	bool last_block;
	struct k_msgq in_queue;
	struct k_msgq out_queue;
};

/* What the stream state machine leaves for the SAI front end to do, which is
 * the only owner of the peripheral registers.
 */
enum i2s_mcux_sai_stream_action {
	/* Leave the stream enabled. */
	I2S_MCUX_SAI_STREAM_RUN,
	/* The callback arrived in a state the stream does not handle. */
	I2S_MCUX_SAI_STREAM_IGNORE,
	/* Gate the bit clock but keep every queued buffer. */
	I2S_MCUX_SAI_STREAM_PAUSE,
	/* Disable the stream and keep every queued buffer. */
	I2S_MCUX_SAI_STREAM_STOP,
	/* Disable the stream and release every queued buffer. */
	I2S_MCUX_SAI_STREAM_STOP_DROP,
	/* Disable the stream, release the in-flight buffers only. */
	I2S_MCUX_SAI_STREAM_STOP_DRAIN,
};

void i2s_mcux_sai_stream_purge(struct i2s_mcux_sai_stream *strm, bool in_drop, bool out_drop);

int i2s_mcux_sai_stream_dma_width(uint8_t word_size_bits, uint8_t *width);

uint32_t i2s_mcux_sai_stream_channel_mask(uint32_t channel_mask);

uint8_t i2s_mcux_sai_stream_min_buffers(enum i2s_dir dir);

struct i2s_mcux_sai_tx_fifo_config
i2s_mcux_sai_stream_tx_fifo_config(uint32_t fifo_count, uint8_t word_size_bytes,
				   bool is_sdma);

int i2s_mcux_sai_stream_tx_reload(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				  uint32_t dest_address, uint8_t *blocks_queued);

int i2s_mcux_sai_stream_tx_start(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				 uint32_t dest_address);

int i2s_mcux_sai_stream_rx_start(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				 uint32_t source_address);

enum i2s_mcux_sai_stream_action
i2s_mcux_sai_stream_tx_complete(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				uint32_t dest_address, int status);

enum i2s_mcux_sai_stream_action
i2s_mcux_sai_stream_rx_complete(struct i2s_mcux_sai_stream *strm, const struct device *dma_dev,
				uint32_t source_address, int status);

#endif /* ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_STREAM_H_ */
