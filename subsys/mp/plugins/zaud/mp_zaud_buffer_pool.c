/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>
#include <zephyr/mp/zaud/mp_zaud_buffer_pool.h>

LOG_MODULE_REGISTER(mp_zaud_buffer_pool, CONFIG_MP_LOG_LEVEL);

static int mp_zaud_buffer_pool_config(struct mp_buffer_pool *pool, struct mp_structure *config)
{
	struct mp_zaud_buffer_pool *zaud_pool = MP_ZAUD_BUFFER_POOL(pool);
	uint32_t block_size;
	uint32_t num_blocks;

	if (config == NULL) {
		return -EINVAL;
	}

	int sample_rate = mp_value_get_int(mp_structure_get_value(config, MP_CAPS_SAMPLE_RATE));
	int bit_width = mp_value_get_int(mp_structure_get_value(config, MP_CAPS_BITWIDTH));
	int num_of_channel =
		mp_value_get_int(mp_structure_get_value(config, MP_CAPS_NUM_OF_CHANNEL));
	int frame_interval =
		mp_value_get_int(mp_structure_get_value(config, MP_CAPS_FRAME_INTERVAL));
	int buffer_count = mp_value_get_int(mp_structure_get_value(config, MP_CAPS_BUFFER_COUNT));

	if (sample_rate <= 0 || bit_width <= 0 || num_of_channel <= 0 || frame_interval <= 0) {
		LOG_ERR("Invalid audio caps for buffer pool");
		return -EINVAL;
	}

	if (zaud_pool->mem_slab == NULL || zaud_pool->slab_backing == NULL) {
		LOG_ERR("Buffer pool storage not wired by element");
		return -EINVAL;
	}

	/* One audio frame in bytes */
	zaud_pool->frame_size =
		mp_zaud_frame_size(bit_width, sample_rate, frame_interval, num_of_channel);

	pool->config.size = zaud_pool->frame_size;
	pool->config.align = bit_width / BITS_PER_BYTE;
	pool->config.min_buffers = (buffer_count > 0) ? (uint8_t)buffer_count : 2;

	/* The DMA slab block must be word-aligned and large enough for one frame */
	block_size = ROUND_UP(zaud_pool->frame_size, sizeof(void *));
	if (block_size == 0 || block_size > zaud_pool->slab_backing_size) {
		LOG_ERR("Frame size %u too large for slab backing %zu", zaud_pool->frame_size,
			zaud_pool->slab_backing_size);
		return -ENOMEM;
	}

	num_blocks = zaud_pool->slab_backing_size / block_size;
	if (zaud_pool->num_blocks != 0) {
		num_blocks = MIN(num_blocks, zaud_pool->num_blocks);
	}

	if (k_mem_slab_init(zaud_pool->mem_slab, zaud_pool->slab_backing, block_size, num_blocks) <
	    0) {
		LOG_ERR("Failed to init audio mem slab (block %u, num %u)", block_size, num_blocks);
		return -ENOMEM;
	}

	LOG_DBG("pool configured: frame_size=%u block=%u blocks=%u", zaud_pool->frame_size,
		block_size, num_blocks);

	return 0;
}

static int mp_zaud_buffer_pool_stop(struct mp_buffer_pool *pool)
{
	/* Slab backing is statically owned by the element; nothing to free. */
	ARG_UNUSED(pool);
	return 0;
}

void mp_zaud_buffer_pool_init(struct mp_buffer_pool *pool)
{
	pool->configure = mp_zaud_buffer_pool_config;
	pool->stop = mp_zaud_buffer_pool_stop;
	/* start / acquire_buffer / release_buffer are provided by the concrete element */
	pool->start = NULL;
	pool->acquire_buffer = NULL;
	pool->release_buffer = NULL;
	pool->started = false;
}
