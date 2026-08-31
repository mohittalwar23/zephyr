/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_CLOCK_H_
#define ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_CLOCK_H_

#include <zephyr/drivers/clock_control/nxp_clock_control.h>

typedef void (*dma_nxp_sdma_hw_init_t)(void *context);

static inline int dma_nxp_sdma_clocked_init(const struct nxp_clock_dt_spec *clock,
					    dma_nxp_sdma_hw_init_t hw_init, void *context)
{
	int ret;

	if (clock->dev != NULL) {
		if (!nxp_clock_is_ready_dt(clock)) {
			return -ENODEV;
		}

		ret = nxp_clock_control_on_dt(clock);
		if (ret < 0) {
			return ret;
		}
	}

	hw_init(context);

	return 0;
}

#endif /* ZEPHYR_DRIVERS_DMA_DMA_NXP_SDMA_CLOCK_H_ */
