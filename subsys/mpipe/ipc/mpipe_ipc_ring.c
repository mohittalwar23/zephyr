/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_ring.h>

static volatile uint32_t *producer_sequence(struct mpipe_ipc_ring *ring)
{
	return &ring->shared->producer.sequence;
}

static volatile uint32_t *consumer_sequence(struct mpipe_ipc_ring *ring)
{
	return &ring->shared->consumer.sequence;
}

/*
 * The peer's counter. Ours is mirrored locally and never read back: re-reading
 * a value this core is the only writer of would add a shared-memory round trip
 * per period and could only ever return what we last stored.
 */
static uint32_t peer_sequence(struct mpipe_ipc_ring *ring)
{
	return ring->is_producer ? ring->ops->load(consumer_sequence(ring))
				 : ring->ops->load(producer_sequence(ring));
}

static uint8_t *period_at(struct mpipe_ipc_ring *ring, uint32_t sequence)
{
	/*
	 * The only place a monotonic counter becomes an index. period_count is
	 * not required to be a power of two, so this is a modulo rather than a
	 * mask; it happens once per period, not per sample.
	 */
	uint32_t slot = sequence % ring->period_count;

	return (uint8_t *)ring->shared->data + ((size_t)slot * ring->period_bytes);
}

uint32_t mpipe_ipc_ring_capacity(size_t region_size, uint32_t period_bytes)
{
	if (period_bytes == 0U || region_size <= MPIPE_IPC_RING_DATA_OFFSET) {
		return 0U;
	}

	return (uint32_t)((region_size - MPIPE_IPC_RING_DATA_OFFSET) / period_bytes);
}

int mpipe_ipc_ring_init(struct mpipe_ipc_ring *ring,
			volatile struct mpipe_ipc_ring_shared *shared,
			const struct mpipe_ipc_ring_ops *ops, bool is_producer,
			uint32_t period_bytes, uint32_t period_count,
			size_t region_size)
{
	if (ring == NULL || shared == NULL || ops == NULL) {
		return -EINVAL;
	}
	if (ops->load == NULL || ops->store == NULL) {
		return -EINVAL;
	}
	if (period_bytes == 0U || period_count == 0U) {
		return -EINVAL;
	}
	if (mpipe_ipc_ring_capacity(region_size, period_bytes) < period_count) {
		return -ENOSPC;
	}

	memset(ring, 0, sizeof(*ring));
	ring->shared = shared;
	ring->ops = ops;
	ring->is_producer = is_producer;
	ring->period_bytes = period_bytes;
	ring->period_count = period_count;
	ring->sequence = 0U;

	if (is_producer) {
		/*
		 * The producer owns the start of the stream, so it resets both
		 * counters. Resetting the consumer's is safe only here, before
		 * any period exists for a consumer to be reading.
		 */
		ops->store(consumer_sequence(ring), 0U);
		ops->store(&shared->consumer.underruns, 0U);
		ops->store(producer_sequence(ring), 0U);
		ops->store(&shared->producer.overruns, 0U);

		/* Geometry last: it is what tells the consumer the rest is valid. */
		ops->store(&shared->producer.period_bytes, period_bytes);
		ops->store(&shared->producer.period_count, period_count);

		return 0;
	}

	/*
	 * A geometry mismatch would not fail loudly -- each side would simply
	 * address different bytes than the other wrote -- so check rather than
	 * assume. Nothing in the hardware enforces this agreement.
	 */
	if (ops->load(&shared->producer.period_bytes) == 0U ||
	    ops->load(&shared->producer.period_count) == 0U) {
		return -EAGAIN;
	}
	if (ops->load(&shared->producer.period_bytes) != period_bytes ||
	    ops->load(&shared->producer.period_count) != period_count) {
		return -EPROTO;
	}

	/* Join wherever the producer has already got to, rather than at zero. */
	ring->sequence = ops->load(producer_sequence(ring));
	ops->store(consumer_sequence(ring), ring->sequence);

	return 0;
}

uint32_t mpipe_ipc_ring_fill(const struct mpipe_ipc_ring *ring)
{
	struct mpipe_ipc_ring *mutable_ring = (struct mpipe_ipc_ring *)ring;
	uint32_t written;
	uint32_t read;

	if (ring == NULL) {
		return 0U;
	}

	if (ring->is_producer) {
		written = ring->sequence;
		read = peer_sequence(mutable_ring);
	} else {
		written = peer_sequence(mutable_ring);
		read = ring->sequence;
	}

	/* Unsigned subtraction, so this stays correct across the wrap. */
	return written - read;
}

uint32_t mpipe_ipc_ring_space(const struct mpipe_ipc_ring *ring)
{
	uint32_t fill;

	if (ring == NULL) {
		return 0U;
	}

	fill = mpipe_ipc_ring_fill(ring);

	/*
	 * A fill larger than the ring means the peer's counter is not one this
	 * side can reason about -- a restart it has not noticed yet. Report no
	 * space rather than a wildly wrong number.
	 */
	return (fill > ring->period_count) ? 0U : (ring->period_count - fill);
}

void *mpipe_ipc_ring_claim_write(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || !ring->is_producer || ring->claimed) {
		return NULL;
	}
	if (mpipe_ipc_ring_space(ring) == 0U) {
		return NULL;
	}

	ring->claimed = true;

	return period_at(ring, ring->sequence);
}

int mpipe_ipc_ring_commit_write(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || !ring->is_producer || !ring->claimed) {
		return -EINVAL;
	}

	/*
	 * Advance only after the period is fully written. The consumer treats
	 * this store as the point the data becomes readable, so publishing it
	 * earlier would hand over a half-filled period.
	 */
	ring->sequence++;
	ring->claimed = false;
	ring->ops->store(producer_sequence(ring), ring->sequence);

	return 0;
}

const void *mpipe_ipc_ring_claim_read(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || ring->is_producer || ring->claimed) {
		return NULL;
	}
	if (mpipe_ipc_ring_fill(ring) == 0U) {
		return NULL;
	}

	ring->claimed = true;

	return period_at(ring, ring->sequence);
}

int mpipe_ipc_ring_commit_read(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || ring->is_producer || !ring->claimed) {
		return -EINVAL;
	}

	ring->sequence++;
	ring->claimed = false;
	ring->ops->store(consumer_sequence(ring), ring->sequence);

	return 0;
}

void mpipe_ipc_ring_record_overrun(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || !ring->is_producer) {
		return;
	}

	ring->ops->store(&ring->shared->producer.overruns,
			 ring->ops->load(&ring->shared->producer.overruns) + 1U);
}

void mpipe_ipc_ring_record_underrun(struct mpipe_ipc_ring *ring)
{
	if (ring == NULL || ring->is_producer) {
		return;
	}

	ring->ops->store(&ring->shared->consumer.underruns,
			 ring->ops->load(&ring->shared->consumer.underruns) + 1U);
}
