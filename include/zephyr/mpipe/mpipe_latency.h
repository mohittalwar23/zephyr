/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Pipeline latency instrumentation.
 * @ingroup mpipe_framework
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_
#define ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/**
 * @defgroup mpipe_latency Latency instrumentation
 * @ingroup mpipe
 * @brief Measures where a pipeline's latency actually goes.
 *
 * Two quantities, deliberately not interchangeable:
 *
 * - **Transit** is how long one buffer takes to travel from the source that
 *   produced it to the sink that consumed it. It is the cost of the software
 *   path, and on a linear pipeline it is microseconds.
 * - **Occupancy** is how many of the pool's buffers are allocated at the
 *   source-to-sink handoff. It is a diagnostic of pool pressure, not a
 *   measurement of buffering latency.
 *
 * Occupancy is deliberately reported in blocks rather than converted to time.
 * The measured interval between handoffs is reported alongside it only as a
 * stream-health diagnostic.
 * @{
 */

struct mpipe_element;
struct net_buf;

/** @brief Aggregate transit statistics, in microseconds. */
struct mpipe_latency_stats {
	/** Number of buffers measured. */
	uint32_t count;
	/** Smallest transit seen; 0 when @ref count is 0. */
	uint32_t min_us;
	/** Largest transit seen. */
	uint32_t max_us;
	/** Sum of all measurements; divide by @ref count for the mean. */
	uint64_t sum_us;
};

/**
 * @brief Handoff-time buffer-pool utilisation, in allocated blocks.
 *
 * One sample is taken as a source-owned buffer reaches a sink, before the
 * sink queues or releases it. For a slab-backed audio pool this includes all
 * allocated blocks, including blocks currently owned by a driver. It is a
 * diagnostic of pool pressure at that handoff point, not average queue depth
 * and not a measurement of buffering latency.
 */
struct mpipe_latency_occupancy_stats {
	/** Number of samples taken, one per buffer reaching a sink. */
	uint32_t count;
	/** Buffers the pool holds in total, from the most recent sample. */
	uint32_t capacity;
	/** Fewest buffers seen in flight; 0 when @ref count is 0. */
	uint32_t min_blocks;
	/** Most buffers seen in flight. */
	uint32_t max_blocks;
	/** Sum of handoff-time samples; divide by @ref count for their mean. */
	uint64_t sum_blocks;
	/**
	 * Mean interval between buffers, in microseconds, 0 until at least two
	 * have been seen.
	 *
	 * Compare it against the negotiated frame interval before converting
	 * occupancy into time: if they disagree, the stream is not running at
	 * the rate it negotiated and the negotiated figure is the wrong
	 * multiplier.
	 */
	uint32_t period_us;
};

/**
 * @brief Convert a cycle delta to microseconds.
 *
 * Subtracts in unsigned 32-bit arithmetic, so it is correct across one counter
 * wrap. The wrap window is UINT32_MAX / sys_clock_hw_cycles_per_sec, which is
 * only 5.4 s at 800 MHz: never span a whole measurement window with this,
 * accumulate consecutive deltas instead.
 */
static inline uint32_t mpipe_latency_cyc_delta_to_us(uint32_t start, uint32_t end)
{
	uint64_t us = k_cyc_to_us_floor64(end - start);

	return (us > UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

/** @brief Convert accumulated inter-buffer cycles to a mean period. */
static inline uint32_t mpipe_latency_mean_period_us(uint64_t total_cyc, uint32_t gaps)
{
	uint64_t us;

	if (gaps == 0U) {
		return 0U;
	}

	us = k_cyc_to_us_floor64(total_cyc) / gaps;
	return (us > UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

#if defined(CONFIG_MPIPE_LATENCY) || defined(__DOXYGEN__)

/** @brief Stamp a buffer as it leaves a source. */
void mpipe_latency_mark(struct mpipe_element *elem, struct net_buf *buf);

/** @brief Measure a buffer as it reaches a sink, and sample pool occupancy. */
void mpipe_latency_measure(struct mpipe_element *elem, struct net_buf *buf);

/** @brief Read the transit accumulator. */
void mpipe_latency_get_stats(struct mpipe_latency_stats *out);

/** @brief Read the occupancy accumulator. */
void mpipe_latency_occupancy_get_stats(struct mpipe_latency_occupancy_stats *out);

/** @brief Clear every accumulator. */
void mpipe_latency_reset(void);

#else

static inline void mpipe_latency_mark(struct mpipe_element *elem, struct net_buf *buf)
{
	ARG_UNUSED(elem);
	ARG_UNUSED(buf);
}

static inline void mpipe_latency_measure(struct mpipe_element *elem, struct net_buf *buf)
{
	ARG_UNUSED(elem);
	ARG_UNUSED(buf);
}

static inline void mpipe_latency_get_stats(struct mpipe_latency_stats *out)
{
	if (out != NULL) {
		*out = (struct mpipe_latency_stats){0};
	}
}

static inline void mpipe_latency_occupancy_get_stats(struct mpipe_latency_occupancy_stats *out)
{
	if (out != NULL) {
		*out = (struct mpipe_latency_occupancy_stats){0};
	}
}

static inline void mpipe_latency_reset(void)
{
}

#endif /* CONFIG_MPIPE_LATENCY */

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_ */
