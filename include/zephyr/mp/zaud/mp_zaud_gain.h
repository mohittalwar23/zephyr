/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) gain transform element.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_GAIN_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_GAIN_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/mp/core/mp_transform.h>

/** @brief Cast a pointer to a @ref mp_zaud_gain pointer. */
#define MP_ZAUD_GAIN(self) ((struct mp_zaud_gain *)self)

/**
 * @brief Audio gain transform element structure.
 *
 * In-place transform applying a percentage-based gain to PCM samples using
 * Q16.16 fixed-point arithmetic.
 */
struct mp_zaud_gain {
	/** Base transform element */
	struct mp_transform transform;
	/** Gain level as a percentage (0-1000, where 100 == unity gain) */
	int gain_percent;
	/** Gain value in Q16.16 fixed-point format */
	int32_t gain_fixed;
	/** Mute flag - when true, output is zeroed */
	bool mute;
};

/**
 * @brief Initialize an audio gain element.
 *
 * @param self Pointer to the @ref mp_element to initialize as a gain element.
 */
void mp_zaud_gain_init(struct mp_element *self);

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_GAIN_H_ */
