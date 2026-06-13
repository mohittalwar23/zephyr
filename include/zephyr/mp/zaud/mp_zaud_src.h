/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) base source element.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_SRC_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_SRC_H_

#include <zephyr/device.h>

#include <zephyr/audio/audio_caps.h>
#include <zephyr/mp/core/mp_src.h>

#include <zephyr/mp/zaud/mp_zaud_buffer_pool.h>

/** @brief Cast a pointer to a @ref mp_zaud_src pointer. */
#define MP_ZAUD_SRC(self) ((struct mp_zaud_src *)self)

/**
 * @brief Audio source element structure.
 *
 * Base audio source. Concrete sources (e.g. DMIC) extend this and provide the
 * @ref get_audio_caps callback used during caps negotiation.
 */
struct mp_zaud_src {
	/** Base source element */
	struct mp_src src;
	/** Query the supported audio caps of the underlying device */
	int (*get_audio_caps)(const struct device *dev, struct audio_caps *caps);
};

/**
 * @brief Initialize an audio source element.
 *
 * @param self Pointer to the @ref mp_element to initialize as an audio source.
 */
void mp_zaud_src_init(struct mp_element *self);

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_SRC_H_ */
