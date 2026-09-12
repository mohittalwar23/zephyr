/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Shared-DDR audio ring for the direct M7 to HiFi4 link.
 *
 * Bulk audio does not travel on the control path. Three production stacks agree
 * on this split -- the NXP SDK, NXP's own Linux side, and SOF -- and the reason
 * is the same in each: a strictly one-outstanding control channel must not be
 * shared with a real-time bulk stream. Control messages negotiate the format
 * and start and stop the stream; the samples themselves move through here.
 *
 * The ring is single-producer, single-consumer, and each side writes exactly
 * one sequence counter that the other only reads. That is what removes the need
 * for a lock: every field is a naturally aligned 32-bit word with exactly one
 * writer, the same argument the bring-up control block rests on, and the window
 * is uncached on both cores so a plain volatile access is the whole contract.
 *
 * **The counters are monotonic, not wrapped indices.** `write - read` is then
 * the fill level directly, correct across wrap in unsigned arithmetic, and full
 * is distinguishable from empty without sacrificing a slot or keeping a
 * separate flag. Wrapping happens only when a counter is turned into an offset.
 *
 * **The producer publishes the geometry it is using.** The two cores must agree
 * on the period size and the number of periods, and nothing in the hardware
 * enforces that: a mismatch would simply have each side reading different bytes
 * than the other wrote. That failure is silent and was expensive to find once
 * already on this link, in the mailbox channel mapping, so here the consumer
 * checks the producer's published geometry against its own and refuses to start
 * if they differ.
 *
 * There is no doorbell. The consumer polls its sequence against the producer's,
 * which costs one uncached read per period and keeps every audio-related byte
 * off the message path -- the property this split exists to preserve. A
 * notification can be added later if idle power matters more than that.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_RING_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_RING_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bytes reserved for one side's published ring state.
 *
 * Padded for the same reason as the bring-up control block: the window is
 * uncached so it is not required for correctness, but the two cores never share
 * a line if it is ever made cacheable, and it is sized for the larger of the
 * two line sizes (the HiFi4's 128 bytes, not the M7's 32).
 */
#define MPIPE_IPC_RING_BLOCK_SIZE 128U

/** @brief Published by the producer; read by the consumer. */
struct mpipe_ipc_ring_producer {
	/** Periods committed since the stream started. Monotonic. */
	uint32_t sequence;
	/** Bytes in one period. */
	uint32_t period_bytes;
	/** Periods the ring holds. */
	uint32_t period_count;
	/** Periods the producer had to drop because the ring was full. */
	uint32_t overruns;
};

/** @brief Published by the consumer; read by the producer. */
struct mpipe_ipc_ring_consumer {
	/** Periods consumed since the stream started. Monotonic. */
	uint32_t sequence;
	/** Times the consumer wanted a period and the ring was empty. */
	uint32_t underruns;
};

/**
 * @brief The shared ring: two padded state blocks, then the periods.
 *
 * Laid out so the data begins on its own padded boundary, which keeps a period
 * from sharing a line with either side's counters.
 */
struct mpipe_ipc_ring_shared {
	struct mpipe_ipc_ring_producer producer;
	uint8_t producer_pad[MPIPE_IPC_RING_BLOCK_SIZE -
			     sizeof(struct mpipe_ipc_ring_producer)];
	struct mpipe_ipc_ring_consumer consumer;
	uint8_t consumer_pad[MPIPE_IPC_RING_BLOCK_SIZE -
			     sizeof(struct mpipe_ipc_ring_consumer)];
	/** The periods. Sized by the region, not by this declaration. */
	uint8_t data[];
};

/** @brief Offset of the first period from the start of the region. */
#define MPIPE_IPC_RING_DATA_OFFSET (2U * MPIPE_IPC_RING_BLOCK_SIZE)

BUILD_ASSERT(offsetof(struct mpipe_ipc_ring_shared, data) ==
		     MPIPE_IPC_RING_DATA_OFFSET,
	     "the periods must start after both padded state blocks");

/** @brief Accessors for the shared window, injected so this can be host-tested. */
struct mpipe_ipc_ring_ops {
	uint32_t (*load)(const volatile uint32_t *address);
	void (*store)(volatile uint32_t *address, uint32_t value);
};

/** @brief One side's view of the ring. */
struct mpipe_ipc_ring {
	volatile struct mpipe_ipc_ring_shared *shared;
	const struct mpipe_ipc_ring_ops *ops;
	uint32_t period_bytes;
	uint32_t period_count;
	/** This side's own sequence, mirrored locally so it is never re-read. */
	uint32_t sequence;
	bool is_producer;
	/** True between a successful claim and its commit. */
	bool claimed;
};

/**
 * @brief Bind to the shared ring.
 *
 * The producer publishes the geometry and resets both sequences, because it
 * owns the start of the stream. The consumer reads the geometry back and
 * refuses to start unless it matches what it was configured for.
 *
 * @param region_size Bytes available at @p shared, including the state blocks.
 *
 * @retval 0 on success.
 * @retval -EINVAL   A NULL argument, an incomplete @p ops, or a zero geometry.
 * @retval -ENOSPC   @p region_size cannot hold the requested geometry.
 * @retval -EAGAIN   Consumer only: the producer has not published a geometry.
 * @retval -EPROTO   Consumer only: the producer's geometry is not ours.
 */
int mpipe_ipc_ring_init(struct mpipe_ipc_ring *ring,
			volatile struct mpipe_ipc_ring_shared *shared,
			const struct mpipe_ipc_ring_ops *ops, bool is_producer,
			uint32_t period_bytes, uint32_t period_count,
			size_t region_size);

/** @brief Periods currently in the ring. */
uint32_t mpipe_ipc_ring_fill(const struct mpipe_ipc_ring *ring);

/** @brief Periods that can still be written. */
uint32_t mpipe_ipc_ring_space(const struct mpipe_ipc_ring *ring);

/**
 * @brief Claim the next period to write into, without copying.
 *
 * The caller fills the returned buffer -- a DMA engine may write straight into
 * it -- and then calls mpipe_ipc_ring_commit_write(). Nothing is visible to the
 * consumer until that commit.
 *
 * @return A pointer to @c period_bytes of storage, or NULL if the ring is full
 *         or a claim is already outstanding.
 */
void *mpipe_ipc_ring_claim_write(struct mpipe_ipc_ring *ring);

/**
 * @brief Publish the claimed period.
 *
 * @retval 0 on success.
 * @retval -EINVAL @p ring is NULL, is not the producer, or has no claim open.
 */
int mpipe_ipc_ring_commit_write(struct mpipe_ipc_ring *ring);

/**
 * @brief Claim the oldest unread period, without copying.
 *
 * Valid until mpipe_ipc_ring_commit_read(), after which the producer may
 * overwrite it.
 *
 * @return A pointer to @c period_bytes of data, or NULL if the ring is empty or
 *         a claim is already outstanding.
 */
const void *mpipe_ipc_ring_claim_read(struct mpipe_ipc_ring *ring);

/**
 * @brief Release the claimed period back to the producer.
 *
 * @retval 0 on success.
 * @retval -EINVAL @p ring is NULL, is not the consumer, or has no claim open.
 */
int mpipe_ipc_ring_commit_read(struct mpipe_ipc_ring *ring);

/**
 * @brief Record that the producer dropped a period because the ring was full.
 *
 * Kept separate from the claim, because whether a full ring is an error or an
 * expected loss is the stream's policy -- SOF's @c overrun_permitted -- and not
 * something the ring can decide.
 */
void mpipe_ipc_ring_record_overrun(struct mpipe_ipc_ring *ring);

/** @brief Record that the consumer wanted a period and the ring was empty. */
void mpipe_ipc_ring_record_underrun(struct mpipe_ipc_ring *ring);

/** @brief Largest period count a region of @p region_size can hold. */
uint32_t mpipe_ipc_ring_capacity(size_t region_size, uint32_t period_bytes);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_RING_H_ */
