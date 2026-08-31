/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_dai_sai

#include <zephyr/drivers/dai.h>
#include <zephyr/dt-bindings/clock/imx_ccm.h>

#include "sai.h"

BUILD_ASSERT(DT_INST_PROP_LEN(0, clocks) == 2);
BUILD_ASSERT(DT_SAME_NODE(DT_INST_CLOCKS_CTLR_BY_IDX(0, 0), DT_NODELABEL(ccm)));
BUILD_ASSERT(IMX_CCM_SAI3_CLK == 0x0B02U);
BUILD_ASSERT(DT_INST_CLOCKS_CELL_BY_IDX(0, 0, name) == 0x0B02U);
BUILD_ASSERT(DT_SAME_NODE(DT_INST_CLOCKS_CTLR_BY_IDX(0, 1),
			  DT_NODELABEL(sai3_mclk1)));
BUILD_ASSERT(DT_INST_PHA_BY_IDX_OR(0, clocks, 1, name, 0U) == 0U);

int main(void)
{
	return 0;
}
