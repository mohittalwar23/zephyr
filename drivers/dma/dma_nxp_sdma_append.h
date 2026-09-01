/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_APPEND_H_
#define ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_APPEND_H_

#include <zephyr/drivers/dma.h>
#include "dma_nxp_sdma_accounting.h"

struct dma_nxp_sdma_append_state {
	uint32_t write_index;
	uint32_t pending_count;
	uint32_t pending_bytes;
	uint64_t total_copied;
	uint32_t size[DMA_NXP_SDMA_BD_COUNT];
};

struct dma_nxp_sdma_append_slot {
	uint32_t index;
	bool last;
	bool wrap;
};

int dma_nxp_sdma_append_prepare(struct dma_nxp_sdma_append_state *state,
				const struct dma_nxp_sdma_descriptor_state *descriptors);
int dma_nxp_sdma_append_complete(struct dma_nxp_sdma_append_state *state,
				 dma_nxp_sdma_descriptor_owned_t owned, void *context,
				 uint32_t *completed);
int dma_nxp_sdma_append_reload(struct dma_nxp_sdma_append_state *state, bool enabled,
			       size_t size, struct dma_nxp_sdma_append_slot *slot,
			       bool *restart);
void dma_nxp_sdma_append_status(const struct dma_nxp_sdma_append_state *state, bool enabled,
				uint32_t direction, struct dma_status *status);

#endif /* ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_APPEND_H_ */
