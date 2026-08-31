/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/clock/imx_ccm.h>
#include <zephyr/kernel.h>

/*
 * Each active audio peripheral must name the CCM provider together with the
 * clock it owns. The SoC attachment identities are checked separately by
 * verify_soc_symbols.py against the compiled object.
 */

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(sai3))
BUILD_ASSERT(DT_NUM_CLOCKS(DT_NODELABEL(sai3)) == 2);
BUILD_ASSERT(DT_SAME_NODE(DT_CLOCKS_CTLR_BY_IDX(DT_NODELABEL(sai3), 0), DT_NODELABEL(ccm)));
BUILD_ASSERT(DT_CLOCKS_CELL_BY_IDX(DT_NODELABEL(sai3), 0, name) == IMX_CCM_SAI3_CLK);
BUILD_ASSERT(!DT_SAME_NODE(DT_CLOCKS_CTLR_BY_IDX(DT_NODELABEL(sai3), 0),
			   DT_CLOCKS_CTLR_BY_IDX(DT_NODELABEL(sai3), 1)));
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(micfil))
BUILD_ASSERT(DT_NODE_HAS_PROP(DT_NODELABEL(micfil), clocks));
BUILD_ASSERT(DT_SAME_NODE(DT_CLOCKS_CTLR_BY_IDX(DT_NODELABEL(micfil), 0), DT_NODELABEL(ccm)));
BUILD_ASSERT(DT_CLOCKS_CELL_BY_IDX(DT_NODELABEL(micfil), 0, name) == IMX_CCM_PDM_CLK);
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(sdma3))
BUILD_ASSERT(DT_NODE_HAS_PROP(DT_NODELABEL(sdma3), clocks));
BUILD_ASSERT(DT_SAME_NODE(DT_CLOCKS_CTLR_BY_IDX(DT_NODELABEL(sdma3), 0), DT_NODELABEL(ccm)));
BUILD_ASSERT(DT_CLOCKS_CELL_BY_IDX(DT_NODELABEL(sdma3), 0, name) == IMX_CCM_SDMA3_CLK);
#endif

int main(void)
{
	return 0;
}
