/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Acoustic round-trip latency probe.
 * @ingroup mpipe_aud
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AUD_MPIPE_AUD_LOOPBACK_PROBE_H_
#define ZEPHYR_INCLUDE_MPIPE_AUD_MPIPE_AUD_LOOPBACK_PROBE_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/**
 * @defgroup mpipe_aud_loopback_probe Acoustic round-trip probe
 * @ingroup mpipe_aud
 * @brief Measures the latency the declared query models and the transit meter
 *        cannot see.
 *
 * Transit measures a buffer between elements and pool utilisation diagnoses
 * pressure at the handoff point, but neither crosses the converters: the time
 * a sample spends in the DAC, the analogue path and the ADC is invisible to
 * both. This probe closes the loop through the hardware instead of modelling
 * it.
 *
 * It writes a full-scale burst into one outgoing buffer, notes when the
 * transmitter took it, and then watches incoming buffers for the burst coming
 * back. The gap between the two is the whole round trip: playback buffering,
 * the DAC, whatever the signal travels through outside the chip, the ADC, and
 * capture buffering.
 *
 * The loop has to be closed physically, by a cable from the output back to the
 * input or by a headset whose earphone its own microphone can hear. With the
 * loop open, the burst never returns and the probe reports a timeout rather
 * than a wrong result.
 * @{
 */

/** @brief Outcome of one round-trip measurement. */
struct mpipe_aud_loopback_result {
	/** True once a burst has been emitted and detected. */
	bool valid;
	/** True when the emitted burst was not detected within the buffer budget. */
	bool timed_out;
	/** True while a burst is out and has not yet come back. */
	bool pending;
	/** Round-trip latency in microseconds; meaningful when @ref valid. */
	uint32_t round_trip_us;
	/** Peak sample magnitude that satisfied the detector. */
	uint32_t peak;
	/** Largest magnitude seen since arming, whether or not it triggered. */
	uint32_t noise_floor;
	/** Largest magnitude still arriving while the output was silenced. */
	uint32_t quiet_floor;
	/**
	 * Which sample of the returning buffer first crossed the threshold,
	 * and how many samples that buffer held.
	 *
	 * Detection happens when a buffer arrives, so @ref round_trip_us alone
	 * resolves only to a whole buffer. The burst actually arrived part way
	 * through that buffer's capture window, and the fraction that is still
	 * to come after it, (@ref hit_total - @ref hit_index) / @ref hit_total
	 * of one buffer period, is time the round trip did not take.
	 */
	uint32_t hit_index;
	/** Samples in the buffer the burst was found in. */
	uint32_t hit_total;
};

#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE) || defined(__DOXYGEN__)

/**
 * @brief Arm the probe.
 *
 * The next buffer the sink transmits carries the burst.
 *
 * @param bit_width Sample width in bits, 16 or 32.
 * @param threshold Magnitude a returning sample must exceed to count as the
 *                  burst. Pass 0 for a default derived from @p bit_width.
 *
 * @retval 0 on success, -EINVAL for an unsupported width.
 */
int mpipe_aud_loopback_arm(uint8_t bit_width, uint32_t threshold);

/** @brief Read the most recent result. */
void mpipe_aud_loopback_get(struct mpipe_aud_loopback_result *out);

/**
 * @brief Offer an outgoing buffer to the probe, called by the sink.
 *
 * Overwrites the buffer with the burst and stamps the emission time when the
 * probe is armed. Does nothing otherwise.
 */
void mpipe_aud_loopback_emit(void *data, size_t len);

/**
 * @brief Offer an incoming buffer to the probe, called by the source.
 *
 * Scans for the burst and stamps the detection time when it is found. Does
 * nothing unless a burst is outstanding.
 */
void mpipe_aud_loopback_detect(const void *data, size_t len);

#else

static inline int mpipe_aud_loopback_arm(uint8_t bit_width, uint32_t threshold)
{
	ARG_UNUSED(bit_width);
	ARG_UNUSED(threshold);
	return -ENOSYS;
}

static inline void mpipe_aud_loopback_get(struct mpipe_aud_loopback_result *out)
{
	if (out != NULL) {
		*out = (struct mpipe_aud_loopback_result){0};
	}
}

static inline void mpipe_aud_loopback_emit(void *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
}

static inline void mpipe_aud_loopback_detect(const void *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
}

#endif /* CONFIG_MPIPE_AUD_LOOPBACK_PROBE */

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AUD_MPIPE_AUD_LOOPBACK_PROBE_H_ */
