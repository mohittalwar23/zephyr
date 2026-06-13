/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) DMIC source element.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_DMIC_SRC_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_DMIC_SRC_H_

#include <zephyr/mp/zaud/mp_zaud_buffer_pool.h>
#include <zephyr/mp/zaud/mp_zaud_src.h>

/** @brief Cast a pointer to a @ref mp_zaud_dmic_src pointer. */
#define MP_ZAUD_DMIC_SRC(self) ((struct mp_zaud_dmic_src *)self)

/**
 * @brief Audio DMIC source element structure.
 *
 * Captures PCM audio from a digital microphone device and pushes it downstream.
 * The DMIC device is taken from the `dmic-dev` devicetree alias.
 */
struct mp_zaud_dmic_src {
	/** Base audio source */
	struct mp_zaud_src zaud_src;
	/** Buffer pool managing the capture buffers */
	struct mp_zaud_buffer_pool pool;
};

/**
 * @brief Initialize an audio DMIC source element.
 *
 * @param self Pointer to the @ref mp_element to initialize as a DMIC source.
 */
void mp_zaud_dmic_src_init(struct mp_element *self);

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_DMIC_SRC_H_ */
