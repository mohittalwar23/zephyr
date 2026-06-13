/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) I2S codec sink element.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_I2S_CODEC_SINK_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_I2S_CODEC_SINK_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/mp/core/mp_sink.h>

/** @brief Cast a pointer to a @ref mp_zaud_i2s_codec_sink pointer. */
#define MP_ZAUD_I2S_CODEC_SINK(self) ((struct mp_zaud_i2s_codec_sink *)self)

/**
 * @brief Audio I2S codec sink element structure.
 *
 * Consumes PCM audio and outputs it through an I2S interface to an external
 * codec. The I2S device is taken from the `i2s-node0` devicetree alias and the
 * codec from the `audio_codec` node label.
 */
struct mp_zaud_i2s_codec_sink {
	/** Base sink element */
	struct mp_sink sink;
	/** I2S transmit device */
	const struct device *i2s_dev;
	/** Codec configuration device */
	const struct device *codec_dev;
	/** DMA-capable memory slab used by the I2S driver (wired internally) */
	struct k_mem_slab *mem_slab;
	/** Number of buffers written so far at stream start */
	uint8_t count;
	/** Whether the I2S stream has been started */
	bool started;
};

/**
 * @brief Initialize an audio I2S codec sink element.
 *
 * @param self Pointer to the @ref mp_element to initialize as an I2S codec sink.
 */
void mp_zaud_i2s_codec_sink_init(struct mp_element *self);

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_I2S_CODEC_SINK_H_ */
