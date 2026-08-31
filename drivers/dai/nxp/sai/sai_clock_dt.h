/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_DT_H_
#define ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_DT_H_

#include <zephyr/drivers/clock_control/nxp_clock_control.h>

#define _SAI_GET_CLOCK_SPEC(clock_idx, inst) \
	NXP_CLOCK_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst), clock_idx)

#define _SAI_GET_CLOCK_NAME(clock_idx, inst) \
	DT_INST_PROP_BY_IDX(inst, clock_names, clock_idx)

#define _SAI_CLOCK_SPEC_ARRAY(inst) \
	LISTIFY(DT_INST_PROP_LEN_OR(inst, clocks, 0), _SAI_GET_CLOCK_SPEC, (,), inst)

#define _SAI_CLOCK_NAME_ARRAY(inst) \
	LISTIFY(DT_INST_PROP_LEN_OR(inst, clocks, 0), _SAI_GET_CLOCK_NAME, (,), inst)

#define _SAI_GET_CLOCK_ARRAY(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, clocks), \
		    ({ _SAI_CLOCK_SPEC_ARRAY(inst) }), \
		    ({ }))

#define _SAI_GET_CLOCK_NAMES(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, clocks), \
		    ({ _SAI_CLOCK_NAME_ARRAY(inst) }), \
		    ({ }))

#define SAI_CLOCK_DATA_DECLARE(inst) \
	{ \
		.clocks = (struct nxp_clock_dt_spec[])_SAI_GET_CLOCK_ARRAY(inst), \
		.clock_num = DT_INST_PROP_LEN_OR(inst, clocks, 0), \
		.clock_names = (const char *[])_SAI_GET_CLOCK_NAMES(inst), \
	}

struct sai_clock_data {
	struct nxp_clock_dt_spec *clocks;
	uint32_t clock_num;
	const char **clock_names;
};

#endif /* ZEPHYR_DRIVERS_DAI_NXP_SAI_CLOCK_DT_H_ */
