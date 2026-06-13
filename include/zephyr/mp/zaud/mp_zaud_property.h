/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) plugin property identifiers.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_PROPERTY_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_PROPERTY_H_

#include <zephyr/mp/core/mp_property.h>

/**
 * @brief Audio transform property identifiers
 *
 * These extend the base transform properties.
 */
enum prop_zaud_transform {
	/** Gain control (percentage, 0-1000, 100 == unity gain) */
	PROP_GAIN = PROP_TRANSFORM_LAST + 1,
};

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_PROPERTY_H_ */
