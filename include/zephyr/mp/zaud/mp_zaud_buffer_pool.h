/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) buffer pool.
 *
 * @note Audio drivers (DMIC, I2S) manage their data buffers through a private
 * @ref k_mem_slab and there is currently no common audio buffer-management
 * framework. As a result, the zaud elements copy between the driver slab block
 * and the libMP @ref net_buf transport buffer at each driver boundary. A
 * zero-copy pure-audio path (a shared pool negotiated via propose/decide
 * allocation, backed by RTIO) is future work tracked by
 * https://github.com/zephyrproject-rtos/zephyr/issues/107868
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_BUFFER_POOL_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_BUFFER_POOL_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/mp/core/mp_buffer.h>

/** @brief Cast a pointer to a @ref mp_zaud_buffer_pool pointer. */
#define MP_ZAUD_BUFFER_POOL(self) ((struct mp_zaud_buffer_pool *)self)

/**
 * @brief Audio buffer pool structure.
 *
 * Wraps the base @ref mp_buffer_pool and adds the audio-specific resources
 * needed to bridge a Zephyr audio driver (which uses a @ref k_mem_slab) to the
 * libMP @ref net_buf transport.
 */
struct mp_zaud_buffer_pool {
	/** Base buffer pool */
	struct mp_buffer_pool pool;
	/** Audio device this pool reads from (e.g. a DMIC device) */
	const struct device *zaud_dev;
	/** DMA-capable memory slab used by the audio driver (wired by the element) */
	struct k_mem_slab *mem_slab;
	/** Backing storage for @ref mem_slab (wired by the element, DMA-capable) */
	void *slab_backing;
	/** Total size in bytes of @ref slab_backing */
	size_t slab_backing_size;
	/** Number of blocks to carve @ref slab_backing into */
	uint32_t num_blocks;
	/** Size in bytes of one audio frame, computed during configure */
	uint32_t frame_size;
};

/**
 * @brief Initialize an audio buffer pool.
 *
 * Sets up the generic configure/stop callbacks. The concrete element is
 * responsible for wiring @ref mp_zaud_buffer_pool::mem_slab,
 * @ref mp_zaud_buffer_pool::slab_backing and the base pool's net_buf pool, and
 * for providing the acquire_buffer / start callbacks.
 *
 * @param pool Pointer to the base buffer pool to initialize.
 */
void mp_zaud_buffer_pool_init(struct mp_buffer_pool *pool);

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_BUFFER_POOL_H_ */
