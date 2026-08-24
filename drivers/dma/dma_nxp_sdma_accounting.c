/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/drivers/dma.h>
#include "dma_nxp_sdma_accounting.h"

static bool dma_nxp_sdma_direction_valid(uint32_t direction)
{
	return direction == MEMORY_TO_PERIPHERAL || direction == PERIPHERAL_TO_MEMORY;
}

static bool dma_nxp_sdma_descriptor_state_valid(const struct dma_nxp_sdma_descriptor_state *state)
{
	uint32_t capacity = 0U;

	if (state == NULL || state->bd_count == 0U || state->bd_count > DMA_NXP_SDMA_BD_COUNT) {
		return false;
	}

	for (uint32_t i = 0U; i < state->bd_count; i++) {
		if (state->bd_size[i] == 0U || state->bd_size[i] > UINT16_MAX ||
		    capacity > UINT32_MAX - state->bd_size[i]) {
			return false;
		}
		capacity += state->bd_size[i];
	}

	return state->capacity == capacity;
}

static int dma_nxp_sdma_consume(struct dma_nxp_sdma_descriptor_state *state, uint32_t bytes,
				 bool hardware_completion)
{
	if (bytes > state->stat.pending_length) {
		return -EINVAL;
	}

	state->stat.read_position += bytes;
	state->stat.read_position %= state->capacity;

	if (state->stat.read_position > state->stat.write_position) {
		state->stat.free = state->stat.read_position - state->stat.write_position;
	} else {
		state->stat.free = state->capacity -
			(state->stat.write_position - state->stat.read_position);
	}

	state->stat.pending_length = state->capacity - state->stat.free;
	if (hardware_completion) {
		state->stat.total_copied += bytes;
	}

	return 0;
}

static int dma_nxp_sdma_produce(struct dma_nxp_sdma_descriptor_state *state, uint32_t bytes,
				 bool hardware_completion)
{
	if (bytes > state->stat.free) {
		return -EINVAL;
	}

	state->stat.write_position += bytes;
	state->stat.write_position %= state->capacity;

	if (state->stat.write_position > state->stat.read_position) {
		state->stat.pending_length = state->stat.write_position - state->stat.read_position;
	} else {
		state->stat.pending_length = state->capacity -
			(state->stat.read_position - state->stat.write_position);
	}

	state->stat.free = state->capacity - state->stat.pending_length;
	if (hardware_completion) {
		state->stat.total_copied += bytes;
	}

	return 0;
}

int dma_nxp_sdma_descriptor_init_stat(struct dma_nxp_sdma_descriptor_state *state,
				      uint32_t direction)
{
	if (!dma_nxp_sdma_descriptor_state_valid(state) ||
	    !dma_nxp_sdma_direction_valid(direction)) {
		return -EINVAL;
	}

	state->stat.read_position = 0;
	state->stat.write_position = 0;
	state->stat.total_copied = 0;

	switch (direction) {
	case MEMORY_TO_PERIPHERAL:
		state->stat.pending_length = state->capacity;
		state->stat.free = 0;
		break;
	case PERIPHERAL_TO_MEMORY:
		state->stat.pending_length = 0;
		state->stat.free = state->capacity;
		break;
	}

	return 0;
}

int dma_nxp_sdma_descriptor_prepare(struct dma_nxp_sdma_descriptor_state *state,
				    const struct dma_config *config)
{
	struct dma_nxp_sdma_descriptor_state prepared = { 0 };
	const struct dma_block_config *block;

	if (state == NULL || config == NULL || config->head_block == NULL ||
	    config->block_count == 0U || config->block_count > DMA_NXP_SDMA_BD_COUNT ||
	    !dma_nxp_sdma_direction_valid(config->channel_direction)) {
		return -EINVAL;
	}

	prepared.bd_count = config->block_count;
	block = config->head_block;
	for (uint32_t i = 0U; i < prepared.bd_count; i++) {
		if (block == NULL || block->block_size == 0U || block->block_size > UINT16_MAX ||
		    prepared.capacity > UINT32_MAX - block->block_size) {
			return -EINVAL;
		}

		prepared.bd_size[i] = block->block_size;
		prepared.source_address[i] = block->source_address;
		prepared.dest_address[i] = block->dest_address;
		prepared.capacity += block->block_size;
		block = block->next_block;
	}
	if (block != NULL) {
		return -EINVAL;
	}

	*state = prepared;
	return 0;
}

int dma_nxp_sdma_descriptor_complete(struct dma_nxp_sdma_descriptor_state *state,
				     uint32_t direction, uint32_t next_bd,
				     dma_nxp_sdma_descriptor_rearm_t rearm, void *context)
{
	uint32_t completed_bd;
	uint32_t size;
	int ret;

	if (!dma_nxp_sdma_descriptor_state_valid(state) ||
	    !dma_nxp_sdma_direction_valid(direction) ||
	    next_bd >= state->bd_count || rearm == NULL) {
		return -EINVAL;
	}

	/* MCUX reports the next BD after a completion. */
	completed_bd = (next_bd + state->bd_count - 1U) % state->bd_count;
	size = state->bd_size[completed_bd];

	switch (direction) {
	case MEMORY_TO_PERIPHERAL:
		ret = dma_nxp_sdma_consume(state, size, true);
		break;
	case PERIPHERAL_TO_MEMORY:
		ret = dma_nxp_sdma_produce(state, size, true);
		break;
	}

	if (ret == 0) {
		rearm(context, completed_bd, size);
	}

	return ret;
}

int dma_nxp_sdma_descriptor_reload(struct dma_nxp_sdma_descriptor_state *state,
				   uint32_t direction, uintptr_t source_address,
				   uintptr_t dest_address, uint32_t size)
{
	ARG_UNUSED(source_address);
	ARG_UNUSED(dest_address);

	if (!dma_nxp_sdma_descriptor_state_valid(state) ||
	    !dma_nxp_sdma_direction_valid(direction)) {
		return -EINVAL;
	}

	switch (direction) {
	case MEMORY_TO_PERIPHERAL:
		return dma_nxp_sdma_produce(state, size, false);
	case PERIPHERAL_TO_MEMORY:
		return dma_nxp_sdma_consume(state, size, false);
	default:
		return -EINVAL;
	}
}
