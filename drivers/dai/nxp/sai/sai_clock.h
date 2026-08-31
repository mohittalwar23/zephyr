/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_H_
#define ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_H_

#include <zephyr/drivers/clock_control/nxp_clock_control.h>

/**
 * @brief Release SAI clocks in the reverse order of acquisition.
 *
 * @return 0 or the first release error.
 */
int dai_nxp_sai_clocks_disable(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num);

/**
 * @brief Release SAI clocks while unwinding a failed operation.
 *
 * @param error Error that started the unwind.
 *
 * @return @p error, so a failing release cannot mask the original cause.
 */
int dai_nxp_sai_clocks_release(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num,
			       int error);

/**
 * @brief Acquire all SAI clocks or none of them.
 *
 * On failure the clocks this call already acquired are released in reverse
 * order and the error that stopped the acquisition is returned.
 */
int dai_nxp_sai_clocks_enable(const struct nxp_clock_dt_spec *clocks, uint32_t clock_num);

#endif /* ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_H_ */
