/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Optional source-to-sink pipeline latency measurement.
 * @ingroup mpipe_latency
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_
#define ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_

/**
 * @defgroup mpipe_latency Latency measurement
 * @ingroup mpipe_framework
 * @brief Media-agnostic source-to-sink latency measurement.
 *
 * With @kconfig{CONFIG_MPIPE_LATENCY}, the framework stamps every buffer as it
 * leaves a source and measures how long it takes to reach a sink, over @ref
 * mpipe_push_buffer - the one path every buffer travels. Disabled, every entry
 * point below compiles to nothing.
 * @{
 */

#include <stdint.h>

#include <zephyr/sys/util.h>

struct mpipe_element;
struct net_buf;

/** @brief Aggregate latency statistics, in microseconds. */
struct mpipe_latency_stats {
	/** Number of buffers measured. */
	uint32_t count;
	/** Smallest latency seen; 0 when @ref count is 0. */
	uint32_t min_us;
	/** Largest latency seen. */
	uint32_t max_us;
	/** Most recent measurement. */
	uint32_t last_us;
	/** Sum of all measurements; divide by @ref count for the mean. */
	uint64_t sum_us;
};

/** @brief Declared end-to-end latency bound, in microseconds. */
struct mpipe_latency_bound {
	/** Buffering the pipeline always adds (best case). */
	uint32_t min_us;
	/** Buffering headroom before overrun (worst case, >= min_us). */
	uint32_t max_us;
};

#if defined(CONFIG_MPIPE_LATENCY) || defined(__DOXYGEN__)

/**
 * @brief Stamp a buffer's ingress time. Framework-internal, no-op off a source.
 *
 * @param elem Element pushing the buffer.
 * @param buf  Buffer being pushed.
 */
void mpipe_latency_mark(struct mpipe_element *elem, struct net_buf *buf);

/**
 * @brief Measure a buffer's source-to-sink latency. Framework-internal, no-op off a sink.
 *
 * @param elem Element about to receive the buffer.
 * @param buf  Buffer being delivered.
 */
void mpipe_latency_measure(struct mpipe_element *elem, struct net_buf *buf);

/**
 * @brief Read a snapshot of the accumulated latency statistics.
 * @param out Storage for the snapshot.
 */
void mpipe_latency_get_stats(struct mpipe_latency_stats *out);

/** @brief Clear the accumulated latency statistics. */
void mpipe_latency_reset(void);

/**
 * @brief Declare an element's own end-to-end latency contribution.
 *
 * Called by a source or sink at configure time; contributions accumulate
 * (GStreamer's rule: pipeline min/max latency is the sum of each element's).
 *
 * @param min_us Buffering this element always adds.
 * @param max_us Buffering headroom before overrun.
 */
void mpipe_latency_declare(uint32_t min_us, uint32_t max_us);

/**
 * @brief Read the aggregated end-to-end latency bound.
 * @param out Storage for the snapshot.
 */
void mpipe_latency_get_e2e(struct mpipe_latency_bound *out);

#else /* !CONFIG_MPIPE_LATENCY */

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
	*out = (struct mpipe_latency_stats){0};
}

static inline void mpipe_latency_reset(void)
{
}

static inline void mpipe_latency_declare(uint32_t min_us, uint32_t max_us)
{
	ARG_UNUSED(min_us);
	ARG_UNUSED(max_us);
}

static inline void mpipe_latency_get_e2e(struct mpipe_latency_bound *out)
{
	*out = (struct mpipe_latency_bound){0};
}

#endif /* CONFIG_MPIPE_LATENCY */

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_MPIPE_LATENCY_H_ */
