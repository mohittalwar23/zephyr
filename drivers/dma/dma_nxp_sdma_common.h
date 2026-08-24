/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_
#define ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_

#include <errno.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/nxp_sdma.h>

static inline int dma_nxp_sdma_encode_width(uint32_t width, uint32_t *encoded_width)
{
	if (encoded_width == NULL) {
		return -EINVAL;
	}

	/* The MCUX SDMA command encodes 32-bit transfers as zero. */
	switch (width) {
	case 1U:
	case 2U:
		*encoded_width = width;
		return 0;
	case 4U:
		*encoded_width = 0U;
		return 0;
	default:
		return -EINVAL;
	}
}

static inline bool dma_nxp_sdma_is_append_mode(const struct dma_config *config)
{
	if (config == NULL || !config->cyclic ||
	    !(config->dma_slot & DMA_NXP_SDMA_MODE_APPEND)) {
		return false;
	}

	return config->channel_direction == MEMORY_TO_PERIPHERAL ||
	       config->channel_direction == PERIPHERAL_TO_MEMORY;
}

#endif /* ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_ */
