/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_clock.h"

int dai_nxp_sai_clocks_disable(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num)
{
	int result = 0;

	while (clock_num > 0U) {
		int ret = nxp_clock_control_off_dt(&clocks[--clock_num]);

		if (result == 0 && ret < 0) {
			result = ret;
		}
	}

	return result;
}

int dai_nxp_sai_clocks_release(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num,
			       int error)
{
	(void)dai_nxp_sai_clocks_disable(clocks, clock_num);

	return error;
}

int dai_nxp_sai_clocks_enable(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num)
{
	uint32_t i;

	for (i = 0U; i < clock_num; i++) {
		int ret = nxp_clock_control_on_dt(&clocks[i]);

		if (ret < 0) {
			return dai_nxp_sai_clocks_release(clocks, i, ret);
		}
	}

	return 0;
}
