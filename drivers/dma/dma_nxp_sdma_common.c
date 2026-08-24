/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "dma_nxp_sdma_common.h"

bool dma_nxp_sdma_request_admit(struct dma_nxp_sdma_request_state *state, int channel,
				const uint32_t *event, uint32_t channel_count,
				uint32_t event_count)
{
	if (state == NULL || event == NULL || channel <= 0 || channel >= channel_count ||
	    *event >= event_count) {
		return false;
	}

	state->channel = channel;
	state->event_source = *event;
	state->requested = true;

	return true;
}

int dma_nxp_sdma_request_validate(const struct dma_nxp_sdma_request_state *state,
				  uint32_t channel)
{
	if (state == NULL || !state->requested || state->channel != channel) {
		return -EINVAL;
	}

	return 0;
}

void dma_nxp_sdma_request_release(struct dma_nxp_sdma_request_state *state)
{
	if (state != NULL) {
		memset(state, 0, sizeof(*state));
	}
}

void *dma_nxp_sdma_context_at(const struct dma_nxp_sdma_context_store *store,
			      uint32_t channel)
{
	if (store == NULL || store->base == NULL || store->stride == 0U ||
	    channel >= store->count) {
		return NULL;
	}

	return (uint8_t *)store->base + channel * store->stride;
}

int dma_nxp_sdma_ram_script_claim(struct dma_nxp_sdma_ram_script_state *state,
				  const void *controller, bool required)
{
	k_spinlock_key_t key;
	int ret = 0;

	if (!required) {
		return 0;
	}
	if (state == NULL || controller == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&state->lock);
	if (state->owner == NULL) {
		state->owner = controller;
	} else if (state->owner != controller) {
		ret = -ENOTSUP;
	}
	k_spin_unlock(&state->lock, key);

	return ret;
}

int dma_nxp_sdma_validate_slot(const struct dma_config *config, uint32_t *peripheral,
			       bool *append_mode)
{
	uint32_t selected;
	bool append;

	if (config == NULL || peripheral == NULL || append_mode == NULL) {
		return -EINVAL;
	}

	selected = config->dma_slot & DMA_NXP_SDMA_PERIPHERAL_MASK;
	append = (config->dma_slot & DMA_NXP_SDMA_MODE_APPEND) != 0U;

	switch (selected) {
	case DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP:
		if (config->channel_direction != MEMORY_TO_PERIPHERAL &&
		    config->channel_direction != PERIPHERAL_TO_MEMORY) {
			return -EINVAL;
		}
		break;
	case DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM:
		if (config->channel_direction != PERIPHERAL_TO_MEMORY) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (append && !config->cyclic) {
		return -EINVAL;
	}

	*peripheral = selected;
	*append_mode = append;

	return 0;
}
