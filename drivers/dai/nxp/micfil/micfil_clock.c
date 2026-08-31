/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "micfil_clock.h"

int dai_nxp_micfil_apply_sample_rate(uintptr_t base, const struct dai_nxp_micfil_rate *rate,
				     uint32_t sample_rate,
				     dai_nxp_micfil_set_rate_t set_rate)
{
	return set_rate(base, rate->root_rate, sample_rate);
}
