/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/dma.h>
#include <zephyr/sys/util.h>

#include "dma_nxp_sdma_irq.h"

void dma_nxp_sdma_irq_init(struct dma_nxp_sdma_irq_state *state)
{
	state->pending_blocks = 0U;
	state->pending_error = false;
	state->error_status = DMA_STATUS_BLOCK;
}

void dma_nxp_sdma_irq_stop(dma_nxp_sdma_irq_action_t stop, void *context)
{
	/* IRQ-committed notifications remain queued while hardware stops. */
	stop(context);
}

bool dma_nxp_sdma_irq_take(struct dma_nxp_sdma_irq_state *state,
			   bool error_callback_dis, int *status)
{
	if (state->pending_blocks != 0U) {
		state->pending_blocks--;
		*status = DMA_STATUS_BLOCK;
		return true;
	}

	if (state->pending_error) {
		state->pending_error = false;
		*status = state->error_status;
		return *status >= 0 || !error_callback_dis;
	}

	return false;
}

int dma_nxp_sdma_irq_finalize(struct dma_nxp_sdma_irq_state *state, uint32_t completed,
			      int completion_status, uint32_t current_bd, uint32_t next_bd,
			      uint32_t bd_count, dma_nxp_sdma_irq_advance_t advance,
			      void *advance_context)
{
	uint32_t advances = 0U;
	int ret = completion_status;

	if (bd_count == 0U || current_bd >= bd_count || next_bd >= bd_count || advance == NULL) {
		ret = ret != 0 ? ret : -EINVAL;
	} else {
		while (current_bd != next_bd && advances < bd_count) {
			current_bd = advance(advance_context);
			advances++;
			if (current_bd >= bd_count) {
				break;
			}
		}
		if (current_bd != next_bd && ret == 0) {
			ret = -EIO;
		}
	}

	state->pending_blocks += completed;
	if (ret != 0) {
		state->error_status = ret;
		state->pending_error = true;
	}

	return ret;
}

int dma_nxp_sdma_irq_complete_descriptors(
	struct dma_nxp_sdma_irq_state *irq, struct dma_nxp_sdma_descriptor_state *descriptors,
	uint32_t direction, dma_nxp_sdma_descriptor_owned_t owned,
	dma_nxp_sdma_descriptor_rearm_t rearm, void *descriptor_context, uint32_t current_bd,
	dma_nxp_sdma_irq_advance_t advance, void *advance_context, uint32_t *completed)
{
	int ret;

	if (completed == NULL) {
		return -EINVAL;
	}
	*completed = 0U;
	ret = dma_nxp_sdma_descriptor_complete(descriptors, direction, owned, rearm,
					       descriptor_context, completed);

	return dma_nxp_sdma_irq_finalize(irq, *completed, ret, current_bd, descriptors->next_bd,
					 descriptors->bd_count, advance, advance_context);
}
