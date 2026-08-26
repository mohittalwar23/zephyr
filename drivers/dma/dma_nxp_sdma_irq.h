/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_NXP_SDMA_IRQ_H_
#define ZEPHYR_DRIVERS_DMA_NXP_SDMA_IRQ_H_

#include "dma_nxp_sdma_accounting.h"

struct dma_nxp_sdma_irq_state {
	uint32_t pending_blocks;
	bool pending_error;
	int error_status;
};

typedef uint32_t (*dma_nxp_sdma_irq_advance_t)(void *context);
typedef void (*dma_nxp_sdma_irq_action_t)(void *context);

void dma_nxp_sdma_irq_init(struct dma_nxp_sdma_irq_state *state);
void dma_nxp_sdma_irq_stop(dma_nxp_sdma_irq_action_t stop, void *context);
bool dma_nxp_sdma_irq_take(struct dma_nxp_sdma_irq_state *state,
			   bool error_callback_dis, int *status);
int dma_nxp_sdma_irq_finalize(struct dma_nxp_sdma_irq_state *state, uint32_t completed,
			      int completion_status, uint32_t current_bd, uint32_t next_bd,
			      uint32_t bd_count, dma_nxp_sdma_irq_advance_t advance,
			      void *advance_context);
int dma_nxp_sdma_irq_complete_descriptors(
	struct dma_nxp_sdma_irq_state *irq, struct dma_nxp_sdma_descriptor_state *descriptors,
	uint32_t direction, dma_nxp_sdma_descriptor_owned_t owned,
	dma_nxp_sdma_descriptor_rearm_t rearm, void *descriptor_context, uint32_t current_bd,
	dma_nxp_sdma_irq_advance_t advance, void *advance_context, uint32_t *completed);

#endif /* ZEPHYR_DRIVERS_DMA_NXP_SDMA_IRQ_H_ */
