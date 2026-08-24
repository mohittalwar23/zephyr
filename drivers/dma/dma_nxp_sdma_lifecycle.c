/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dma_nxp_sdma_lifecycle.h"

void dma_nxp_sdma_lifecycle_start(struct dma_nxp_sdma_lifecycle *lifecycle,
				  dma_nxp_sdma_lifecycle_action_t start, void *context)
{
	k_spinlock_key_t key = k_spin_lock(&lifecycle->lock);

	lifecycle->started = true;
	start(context);
	k_spin_unlock(&lifecycle->lock, key);
}

void dma_nxp_sdma_lifecycle_stop(struct dma_nxp_sdma_lifecycle *lifecycle,
				 dma_nxp_sdma_lifecycle_action_t stop, void *context)
{
	k_spinlock_key_t key = k_spin_lock(&lifecycle->lock);

	lifecycle->started = false;
	stop(context);
	k_spin_unlock(&lifecycle->lock, key);
}

void dma_nxp_sdma_lifecycle_complete(struct dma_nxp_sdma_lifecycle *lifecycle,
				     dma_nxp_sdma_lifecycle_complete_t complete,
				     void *complete_context,
				     dma_nxp_sdma_lifecycle_action_t restart, void *restart_context)
{
	k_spinlock_key_t key = k_spin_lock(&lifecycle->lock);

	if (lifecycle->started && complete(complete_context)) {
		restart(restart_context);
	}
	k_spin_unlock(&lifecycle->lock, key);
}
