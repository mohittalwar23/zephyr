/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DAI_NXP_MICFIL_CLOCK_H_
#define ZEPHYR_DRIVERS_DAI_NXP_MICFIL_CLOCK_H_

#include <zephyr/drivers/clock_control/nxp_clock_control.h>

static inline int dai_nxp_micfil_clock_prepare(const struct nxp_clock_dt_spec *clock,
					       uint32_t *rate)
{
	int ret;

	if (!nxp_clock_is_ready_dt(clock)) {
		return -ENODEV;
	}

	ret = nxp_clock_control_on_dt(clock);
	if (ret < 0) {
		return ret;
	}

	ret = nxp_clock_control_get_rate_dt(clock, rate);
	if (ret < 0) {
		(void)nxp_clock_control_off_dt(clock);
	}

	return ret;
}

#endif /* ZEPHYR_DRIVERS_DAI_NXP_MICFIL_CLOCK_H_ */
