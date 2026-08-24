/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_NXP_SDMA_LIFECYCLE_H_
#define ZEPHYR_DRIVERS_DMA_NXP_SDMA_LIFECYCLE_H_

#include <zephyr/kernel.h>

struct dma_nxp_sdma_lifecycle {
	bool started;
	struct k_spinlock lock;
};

typedef void (*dma_nxp_sdma_lifecycle_action_t)(void *context);
typedef bool (*dma_nxp_sdma_lifecycle_complete_t)(void *context);

void dma_nxp_sdma_lifecycle_start(struct dma_nxp_sdma_lifecycle *lifecycle,
				  dma_nxp_sdma_lifecycle_action_t start, void *context);
void dma_nxp_sdma_lifecycle_stop(struct dma_nxp_sdma_lifecycle *lifecycle,
				 dma_nxp_sdma_lifecycle_action_t stop, void *context);
void dma_nxp_sdma_lifecycle_complete(struct dma_nxp_sdma_lifecycle *lifecycle,
				     dma_nxp_sdma_lifecycle_complete_t complete,
				     void *complete_context,
				     dma_nxp_sdma_lifecycle_action_t restart,
				     void *restart_context);

#endif /* ZEPHYR_DRIVERS_DMA_NXP_SDMA_LIFECYCLE_H_ */
