/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_NXP_SDMA_ACCOUNTING_H_
#define ZEPHYR_DRIVERS_DMA_NXP_SDMA_ACCOUNTING_H_

#include <zephyr/drivers/dma.h>

#ifdef CONFIG_DMA_NXP_SDMA_BD_COUNT
#define DMA_NXP_SDMA_BD_COUNT CONFIG_DMA_NXP_SDMA_BD_COUNT
#else
#define DMA_NXP_SDMA_BD_COUNT 2
#endif

struct dma_nxp_sdma_descriptor_state {
	struct dma_status stat;
	uint32_t bd_size[DMA_NXP_SDMA_BD_COUNT];
	uintptr_t source_address[DMA_NXP_SDMA_BD_COUNT];
	uintptr_t dest_address[DMA_NXP_SDMA_BD_COUNT];
	uint32_t bd_count;
	uint32_t capacity;
	uint32_t next_bd;
};

typedef void (*dma_nxp_sdma_descriptor_rearm_t)(void *context, uint32_t index,
						 uint32_t size);
/*
 * Returns true while a descriptor is still owned by the hardware (not yet
 * completed), false once ownership has returned to software.
 */
typedef bool (*dma_nxp_sdma_descriptor_owned_t)(void *context, uint32_t index);

int dma_nxp_sdma_descriptor_prepare(struct dma_nxp_sdma_descriptor_state *state,
				    const struct dma_config *config);
int dma_nxp_sdma_descriptor_init_stat(struct dma_nxp_sdma_descriptor_state *state,
				      uint32_t direction);
int dma_nxp_sdma_descriptor_complete(struct dma_nxp_sdma_descriptor_state *state,
				     uint32_t direction, dma_nxp_sdma_descriptor_owned_t owned,
				     dma_nxp_sdma_descriptor_rearm_t rearm, void *context,
				     uint32_t *completed);
int dma_nxp_sdma_descriptor_reload(struct dma_nxp_sdma_descriptor_state *state,
				   uint32_t direction, uintptr_t source_address,
				   uintptr_t dest_address, uint32_t size);

#endif
