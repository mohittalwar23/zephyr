/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_DMA_H_
#define ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_DMA_H_

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/nxp_sdma.h>

#define I2S_MCUX_SAI_DMA_CTLR(node_id, dir) DT_DMAS_CTLR_BY_NAME(node_id, dir)
#define I2S_MCUX_SAI_DMA_IS_SDMA(node_id, dir)                                                \
	DT_NODE_HAS_COMPAT(I2S_MCUX_SAI_DMA_CTLR(node_id, dir), nxp_sdma)
#define I2S_MCUX_SAI_DMA_IS_EDMA(node_id, dir)                                                \
	DT_NODE_HAS_COMPAT(I2S_MCUX_SAI_DMA_CTLR(node_id, dir), nxp_mcux_edma)
#define I2S_MCUX_SAI_DMA_SLOT(node_id, dir)                                                   \
	COND_CODE_1(I2S_MCUX_SAI_DMA_IS_SDMA(node_id, dir),                                    \
		((DT_DMAS_CELL_BY_NAME(node_id, dir, mux) | DMA_NXP_SDMA_MODE_APPEND)),          \
		(DT_DMAS_CELL_BY_NAME(node_id, dir, source)))
#define I2S_MCUX_SAI_DMA_REQUEST(node_id, dir)                                                \
	COND_CODE_1(I2S_MCUX_SAI_DMA_IS_SDMA(node_id, dir),                                    \
		(DT_DMAS_CELL_BY_NAME(node_id, dir, channel)), (0U))
#define I2S_MCUX_SAI_DMA_SDMA_MUX_SUPPORTED(node_id, dir)                                     \
	COND_CODE_1(I2S_MCUX_SAI_DMA_IS_SDMA(node_id, dir),                                    \
		((DT_DMAS_CELL_BY_NAME(node_id, dir, mux) ==                                      \
		  DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP) ||                                           \
		 (DT_DMAS_CELL_BY_NAME(node_id, dir, mux) ==                                      \
		  DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM)),                                       \
		(1))

struct i2s_mcux_sai_dma_channel {
	uint32_t channel;
	uint32_t request;
	bool request_channel;
	bool acquired;
};

static inline int i2s_mcux_sai_dma_acquire_channel(
	const struct device *dma_dev, struct i2s_mcux_sai_dma_channel *spec)
{
	int channel;

	if (dma_dev == NULL || spec == NULL) {
		return -EINVAL;
	}

	if (!spec->request_channel) {
		return 0;
	}

	channel = dma_request_channel(dma_dev, &spec->request);
	if (channel < 0) {
		return channel;
	}

	spec->channel = channel;
	spec->acquired = true;

	return 0;
}

static inline void i2s_mcux_sai_dma_release_channel(const struct device *dma_dev,
						    struct i2s_mcux_sai_dma_channel *spec)
{
	if (dma_dev == NULL || spec == NULL || !spec->acquired) {
		return;
	}

	dma_release_channel(dma_dev, spec->channel);
	spec->acquired = false;
}

static inline int i2s_mcux_sai_dma_acquire_pair(const struct device *dma_dev,
						struct i2s_mcux_sai_dma_channel *tx,
						struct i2s_mcux_sai_dma_channel *rx)
{
	int ret;

	if (tx == NULL || rx == NULL) {
		return -EINVAL;
	}

	/* A devicetree controller reference is not a readiness check: the
	 * controller may have failed its own initialization.
	 */
	if (!device_is_ready(dma_dev)) {
		return -ENODEV;
	}

	ret = i2s_mcux_sai_dma_acquire_channel(dma_dev, tx);
	if (ret < 0) {
		return ret;
	}

	ret = i2s_mcux_sai_dma_acquire_channel(dma_dev, rx);
	if (ret < 0) {
		i2s_mcux_sai_dma_release_channel(dma_dev, tx);
		return ret;
	}

	return 0;
}

static inline void i2s_mcux_sai_dma_release_pair(const struct device *dma_dev,
						 struct i2s_mcux_sai_dma_channel *tx,
						 struct i2s_mcux_sai_dma_channel *rx)
{
	i2s_mcux_sai_dma_release_channel(dma_dev, rx);
	i2s_mcux_sai_dma_release_channel(dma_dev, tx);
}

#endif /* ZEPHYR_DRIVERS_I2S_I2S_MCUX_SAI_DMA_H_ */
