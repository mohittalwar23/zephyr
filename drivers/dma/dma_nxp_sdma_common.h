/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_
#define ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/nxp_sdma.h>

struct dma_nxp_sdma_request_state {
	uint32_t channel;
	uint32_t event_source;
	bool requested;
};

struct dma_nxp_sdma_context_store {
	void *base;
	size_t stride;
	uint32_t count;
};

struct dma_nxp_sdma_ram_script_state {
	struct k_spinlock lock;
	const void *owner;
	uint32_t claim_count;
};

static inline int dma_nxp_sdma_encode_width(uint32_t width, uint32_t *encoded_width)
{
	if (encoded_width == NULL) {
		return -EINVAL;
	}

	/* The MCUX SDMA command encodes 32-bit transfers as zero. */
	switch (width) {
	case 1U:
	case 2U:
	case 3U:
		*encoded_width = width;
		return 0;
	case 4U:
		*encoded_width = 0U;
		return 0;
	default:
		return -EINVAL;
	}
}

bool dma_nxp_sdma_request_admit(struct dma_nxp_sdma_request_state *state, int channel,
				const uint32_t *event, uint32_t channel_count,
				uint32_t event_count);
int dma_nxp_sdma_request_validate(const struct dma_nxp_sdma_request_state *state,
				  uint32_t channel);
void dma_nxp_sdma_request_release(struct dma_nxp_sdma_request_state *state);
void *dma_nxp_sdma_context_at(const struct dma_nxp_sdma_context_store *store,
			      uint32_t channel);
int dma_nxp_sdma_ram_script_claim(struct dma_nxp_sdma_ram_script_state *state,
				  const void *controller, bool required, bool *claimed);
void dma_nxp_sdma_ram_script_release(struct dma_nxp_sdma_ram_script_state *state,
				     const void *controller, bool *claimed);
int dma_nxp_sdma_validate_slot(const struct dma_config *config, uint32_t *peripheral,
			       bool *append_mode);

#endif /* ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_COMMON_H_ */
