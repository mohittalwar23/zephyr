/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "dma_nxp_sdma_append.h"

int dma_nxp_sdma_append_prepare(struct dma_nxp_sdma_append_state *state,
				const struct dma_nxp_sdma_descriptor_state *descriptors)
{
	struct dma_nxp_sdma_append_state prepared = {0};
	uint32_t capacity = 0U;

	if (state == NULL || descriptors == NULL || descriptors->bd_count == 0U ||
	    descriptors->bd_count >= DMA_NXP_SDMA_BD_COUNT) {
		return -EINVAL;
	}

	for (uint32_t i = 0U; i < descriptors->bd_count; i++) {
		if (descriptors->bd_size[i] == 0U || descriptors->bd_size[i] > UINT16_MAX ||
		    capacity > UINT32_MAX - descriptors->bd_size[i]) {
			return -EINVAL;
		}
		prepared.size[i] = descriptors->bd_size[i];
		capacity += descriptors->bd_size[i];
	}
	if (capacity != descriptors->capacity) {
		return -EINVAL;
	}

	prepared.write_index = descriptors->bd_count;
	prepared.pending_count = descriptors->bd_count;
	prepared.pending_bytes = capacity;
	*state = prepared;

	return 0;
}

int dma_nxp_sdma_append_complete(struct dma_nxp_sdma_append_state *state,
				 dma_nxp_sdma_descriptor_owned_t owned, void *context,
				 uint32_t *completed)
{
	uint32_t pending = state != NULL ? state->pending_count : 0U;
	uint32_t count = 0U;

	if (state == NULL || owned == NULL || completed == NULL) {
		return -EINVAL;
	}

	/*
	 * One IRQ can coalesce several finished descriptors, so walk the
	 * currently outstanding append slots in order until hardware still
	 * owns the next one. Appended slots are one-shot: a completed slot
	 * is freed here and re-armed later by dma_reload(), not by this scan.
	 */
	while (count < pending) {
		uint32_t index = (state->write_index + DMA_NXP_SDMA_BD_COUNT - pending + count) %
				  DMA_NXP_SDMA_BD_COUNT;
		uint32_t size;

		if (owned(context, index)) {
			break;
		}

		size = state->size[index];
		if (size == 0U || size > state->pending_bytes) {
			state->pending_count -= count;
			*completed = count;
			return -EINVAL;
		}

		state->size[index] = 0U;
		state->pending_bytes -= size;
		state->total_copied += size;
		count++;
	}

	state->pending_count -= count;
	*completed = count;

	return 0;
}

int dma_nxp_sdma_append_reload(struct dma_nxp_sdma_append_state *state, bool enabled,
			       size_t size, struct dma_nxp_sdma_append_slot *slot,
			       bool *restart)
{
	if (state == NULL || slot == NULL || restart == NULL || size == 0U || size > UINT16_MAX ||
	    size > UINT32_MAX - state->pending_bytes) {
		return -EINVAL;
	}
	if (state->pending_count >= DMA_NXP_SDMA_BD_COUNT) {
		return -EBUSY;
	}

	slot->index = state->write_index;
	slot->last = true;
	slot->wrap = state->write_index == DMA_NXP_SDMA_BD_COUNT - 1U;
	state->size[state->write_index] = size;
	state->write_index = (state->write_index + 1U) % DMA_NXP_SDMA_BD_COUNT;
	state->pending_count++;
	state->pending_bytes += size;
	*restart = enabled;

	return 0;
}

void dma_nxp_sdma_append_status(const struct dma_nxp_sdma_append_state *state, bool enabled,
				uint32_t direction, struct dma_status *status)
{
	if (state == NULL || status == NULL) {
		return;
	}

	status->busy = enabled && state->pending_count != 0U;
	status->dir = direction;
	status->free = DMA_NXP_SDMA_BD_COUNT - state->pending_count;
	status->pending_length = state->pending_bytes;
	status->read_position = 0U;
	status->write_position = 0U;
	status->total_copied = state->total_copied;
}
