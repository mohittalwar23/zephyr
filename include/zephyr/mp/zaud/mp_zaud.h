/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mp
 * @brief Audio (zaud) plugin common definitions and utilities.
 */

#ifndef ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_H_
#define ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_H_

#include <stdint.h>

#include <zephyr/audio/audio_caps.h>

struct mp_value;

/**
 * @brief Supported zaud sample rates (Hz)
 * @{
 */
#define MP_ZAUD_SAMPLE_RATE_8000  8000  /**< 8 kHz */
#define MP_ZAUD_SAMPLE_RATE_16000 16000 /**< 16 kHz */
#define MP_ZAUD_SAMPLE_RATE_32000 32000 /**< 32 kHz */
#define MP_ZAUD_SAMPLE_RATE_44100 44100 /**< 44.1 kHz */
#define MP_ZAUD_SAMPLE_RATE_48000 48000 /**< 48 kHz */
#define MP_ZAUD_SAMPLE_RATE_96000 96000 /**< 96 kHz */
/** @} */

/**
 * @brief Supported zaud bit widths
 * @{
 */
#define MP_ZAUD_BIT_WIDTH_16 16 /**< 16 bit */
#define MP_ZAUD_BIT_WIDTH_24 24 /**< 24 bit */
#define MP_ZAUD_BIT_WIDTH_32 32 /**< 32 bit */
/** @} */

/**
 * @brief Convert an audio driver sample-rate mask to a zaud sample-rate value.
 *
 * @param sample_rate_mask Audio driver sample rate mask (one of AUDIO_SAMPLE_RATE_*)
 * @return Corresponding zaud sample rate value in Hz, or 0 if unsupported
 */
uint32_t audio2mp_sample_rate(uint32_t sample_rate_mask);

/**
 * @brief Convert an audio driver bit-width mask to a zaud bit-width value.
 *
 * @param bit_width_mask Audio driver bit width mask (one of AUDIO_BIT_WIDTH_*)
 * @return Corresponding zaud bit width value, or 0 if unsupported
 */
uint32_t audio2mp_bit_width(uint32_t bit_width_mask);

/**
 * @brief Compute the size in bytes of one interleaved PCM audio frame.
 *
 * @param bit_width        Sample width in bits.
 * @param sample_rate      Sample rate in Hz.
 * @param frame_interval_us Frame duration in microseconds.
 * @param channels         Number of channels.
 * @return Frame size in bytes.
 */
uint32_t mp_zaud_frame_size(int bit_width, int sample_rate, int frame_interval_us, int channels);

/**
 * @brief Build an MP_TYPE_LIST of values from an audio capability bitmask.
 *
 * Iterates the set bits of @p mask, converting each `BIT(i)` to a zaud value via
 * @p conv (e.g. @ref audio2mp_sample_rate) and appending it to a new value list.
 *
 * @param mask Audio driver capability bitmask.
 * @param conv Conversion function applied to each set bit's mask value.
 * @return A newly allocated MP_TYPE_LIST value (caller owns it).
 */
struct mp_value *mp_zaud_mask_to_value_list(uint32_t mask, uint32_t (*conv)(uint32_t));

#endif /* ZEPHYR_INCLUDE_MP_ZAUD_MP_ZAUD_H_ */
