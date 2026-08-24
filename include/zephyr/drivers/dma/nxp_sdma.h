/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DMA_NXP_SDMA_H_
#define ZEPHYR_INCLUDE_DRIVERS_DMA_NXP_SDMA_H_

#include <zephyr/sys/util.h>

/* dma_slot bit 7 is reserved for the Zephyr SDMA append-mode contract. */
#define DMA_NXP_SDMA_MODE_APPEND BIT(7)
#define DMA_NXP_SDMA_PERIPHERAL_MASK GENMASK(6, 0)

/* Supported MCUX SDMA peripheral script identifiers. */
#define DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP      5U
#define DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM 6U

/*
 * Every SDMA client, including fixed-ring clients, must obtain a physical
 * channel with dma_request_channel(). The filter parameter is the peripheral
 * event/request ID; it is independent of the returned physical channel.
 */

#endif /* ZEPHYR_INCLUDE_DRIVERS_DMA_NXP_SDMA_H_ */
