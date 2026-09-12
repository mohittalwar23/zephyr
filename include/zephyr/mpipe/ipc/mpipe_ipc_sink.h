/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Pipeline sink that hands audio to a peer core.
 * @ingroup mpipe_ipc_sink
 *
 * This is the end of a pipeline on one core and the start of processing on
 * another. Buffers arriving here are written into the shared audio ring, where
 * the peer's consumer picks them up; nothing travels on the control path.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SINK_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SINK_H_

/**
 * @defgroup mpipe_ipc_sink IPC sink
 * @ingroup mpipe_ipc
 * @brief Sink element that forwards audio to a peer core over shared memory.
 * @{
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_ring.h>
#include <zephyr/mpipe/mpipe_pad.h>
#include <zephyr/mpipe/mpipe_sink.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IPC sink element.
 *
 * The ring is supplied at init rather than negotiated, because its geometry is
 * a property of the link the two cores already agreed on, not of the pipeline.
 */
struct mpipe_ipc_sink {
	/** Base sink element. */
	struct mpipe_sink sink;
	/** The shared ring, already opened as a producer. */
	struct mpipe_ipc_ring *ring;
	/** Channels arriving from upstream. */
	uint32_t channels;
	/** Bytes per sample arriving from upstream. */
	uint32_t sample_bytes;
	/** Samples the peer expects in one period, from the ring's geometry. */
	uint32_t period_samples;
	/** True once the format has been accepted. */
	bool configured;
	/** Periods the peer could not keep up with. */
	uint32_t dropped;
};

/**
 * @brief Initialise an IPC sink.
 *
 * @param ipc_sink Element to initialise.
 * @param id       Element identifier within the pipeline.
 * @param ring     Ring this sink produces into, already initialised.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument.
 */
int mpipe_ipc_sink_init(struct mpipe_ipc_sink *ipc_sink, uint8_t id,
			struct mpipe_ipc_ring *ring);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SINK_H_ */
