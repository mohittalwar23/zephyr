/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_ring.h>

ZTEST_SUITE(mpipe_ipc_ring, NULL, NULL, NULL, NULL, NULL);

#define PERIOD_BYTES 64U
#define PERIOD_COUNT 4U
#define REGION_SIZE  (MPIPE_IPC_RING_DATA_OFFSET + (PERIOD_BYTES * PERIOD_COUNT))

/*
 * Both sides are driven against one region, because the property worth testing
 * is that a producer and a consumer agree -- which cannot be observed from
 * either one alone.
 */
static uint8_t region[REGION_SIZE] __aligned(4);

static uint32_t fake_load(const volatile uint32_t *address)
{
	return *address;
}

static void fake_store(volatile uint32_t *address, uint32_t value)
{
	*address = value;
}

static const struct mpipe_ipc_ring_ops ops = {
	.load = fake_load,
	.store = fake_store,
};

static volatile struct mpipe_ipc_ring_shared *shared(void)
{
	return (volatile struct mpipe_ipc_ring_shared *)region;
}

static void reset_region(void)
{
	memset(region, 0, sizeof(region));
}

/* Write one period whose every byte is `tag`. */
static void produce_tagged(struct mpipe_ipc_ring *p, uint8_t tag)
{
	void *slot = mpipe_ipc_ring_claim_write(p);

	zassert_not_null(slot);
	memset(slot, tag, PERIOD_BYTES);
	zassert_ok(mpipe_ipc_ring_commit_write(p));
}

/* Read one period and assert every byte is `tag`. */
static void consume_expecting(struct mpipe_ipc_ring *c, uint8_t tag)
{
	const uint8_t *slot = mpipe_ipc_ring_claim_read(c);

	zassert_not_null(slot);
	for (size_t i = 0; i < PERIOD_BYTES; i++) {
		zassert_equal(slot[i], tag, "byte %zu of the period is wrong", i);
	}
	zassert_ok(mpipe_ipc_ring_commit_read(c));
}

static void open_pair(struct mpipe_ipc_ring *p, struct mpipe_ipc_ring *c)
{
	reset_region();
	zassert_ok(mpipe_ipc_ring_init(p, shared(), &ops, true, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));
	zassert_ok(mpipe_ipc_ring_init(c, shared(), &ops, false, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));
}

ZTEST(mpipe_ipc_ring, test_a_period_arrives_intact)
{
	struct mpipe_ipc_ring producer, consumer;

	open_pair(&producer, &consumer);

	zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U);
	zassert_equal(mpipe_ipc_ring_space(&producer), PERIOD_COUNT);

	produce_tagged(&producer, 0xa5);
	zassert_equal(mpipe_ipc_ring_fill(&consumer), 1U,
		      "the consumer sees the period as soon as it is committed");

	consume_expecting(&consumer, 0xa5);
	zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U);
	zassert_equal(mpipe_ipc_ring_space(&producer), PERIOD_COUNT);
}

ZTEST(mpipe_ipc_ring, test_nothing_is_visible_before_the_commit)
{
	struct mpipe_ipc_ring producer, consumer;
	void *slot;

	open_pair(&producer, &consumer);

	slot = mpipe_ipc_ring_claim_write(&producer);
	zassert_not_null(slot);
	memset(slot, 0x5a, PERIOD_BYTES);

	zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U,
		      "a claimed but uncommitted period must not be readable");
	zassert_is_null(mpipe_ipc_ring_claim_read(&consumer));

	zassert_ok(mpipe_ipc_ring_commit_write(&producer));
	zassert_equal(mpipe_ipc_ring_fill(&consumer), 1U);
}

ZTEST(mpipe_ipc_ring, test_the_ring_fills_and_refuses_to_overwrite)
{
	struct mpipe_ipc_ring producer, consumer;

	open_pair(&producer, &consumer);

	for (uint8_t i = 0; i < PERIOD_COUNT; i++) {
		produce_tagged(&producer, (uint8_t)(0x10 + i));
	}

	zassert_equal(mpipe_ipc_ring_space(&producer), 0U);
	zassert_is_null(mpipe_ipc_ring_claim_write(&producer),
			"a full ring must not hand out a slot the consumer still owns");

	mpipe_ipc_ring_record_overrun(&producer);
	zassert_equal(shared()->producer.overruns, 1U);

	/* Freeing one slot admits exactly one more period. */
	consume_expecting(&consumer, 0x10);
	zassert_equal(mpipe_ipc_ring_space(&producer), 1U);
	produce_tagged(&producer, 0x20);
	zassert_equal(mpipe_ipc_ring_space(&producer), 0U);
}

ZTEST(mpipe_ipc_ring, test_an_empty_ring_yields_nothing)
{
	struct mpipe_ipc_ring producer, consumer;

	open_pair(&producer, &consumer);

	zassert_is_null(mpipe_ipc_ring_claim_read(&consumer));
	mpipe_ipc_ring_record_underrun(&consumer);
	zassert_equal(shared()->consumer.underruns, 1U);
	zassert_equal(mpipe_ipc_ring_commit_read(&consumer), -EINVAL,
		      "committing without a claim is a caller error");
}

/*
 * The counters are monotonic and 32 bits wide, so they wrap long before any
 * realistic stream ends -- at 10 ms periods, after about 16 months. Fill is a
 * subtraction precisely so that the wrap is not a special case.
 */
ZTEST(mpipe_ipc_ring, test_the_sequence_wrap_is_not_a_special_case)
{
	struct mpipe_ipc_ring producer, consumer;

	open_pair(&producer, &consumer);

	/* Park both sides three periods short of the wrap. */
	producer.sequence = UINT32_MAX - 2U;
	consumer.sequence = UINT32_MAX - 2U;
	fake_store(&shared()->producer.sequence, producer.sequence);
	fake_store(&shared()->consumer.sequence, consumer.sequence);

	zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U);
	zassert_equal(mpipe_ipc_ring_space(&producer), PERIOD_COUNT);

	/* Walk across the wrap, checking the data survives the slot mapping. */
	for (uint8_t i = 0; i < 8U; i++) {
		produce_tagged(&producer, (uint8_t)(0x80 + i));
		zassert_equal(mpipe_ipc_ring_fill(&consumer), 1U);
		consume_expecting(&consumer, (uint8_t)(0x80 + i));
		zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U);
	}

	zassert_true(producer.sequence < 8U, "the counter really did wrap");
}

/*
 * A geometry mismatch is silent in hardware: each side simply addresses
 * different bytes than the other wrote. The same class of silent disagreement
 * in the mailbox channel mapping cost a long debugging session on this link, so
 * the consumer checks rather than assumes.
 */
ZTEST(mpipe_ipc_ring, test_a_consumer_refuses_a_geometry_it_does_not_share)
{
	struct mpipe_ipc_ring producer, consumer;

	reset_region();
	zassert_ok(mpipe_ipc_ring_init(&producer, shared(), &ops, true, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));

	/*
	 * Both geometries below fit the region, so nothing local rejects them.
	 * Only the comparison against what the producer published does -- which
	 * is the whole point: a mismatch that fits is the one that stays silent.
	 */
	zassert_equal(mpipe_ipc_ring_init(&consumer, shared(), &ops, false,
					  PERIOD_BYTES * 2U, PERIOD_COUNT / 2U,
					  REGION_SIZE),
		      -EPROTO, "a different period size must not be accepted");
	zassert_equal(mpipe_ipc_ring_init(&consumer, shared(), &ops, false, PERIOD_BYTES,
					  PERIOD_COUNT - 1U, REGION_SIZE),
		      -EPROTO, "nor a different period count");

	zassert_ok(mpipe_ipc_ring_init(&consumer, shared(), &ops, false, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));
}

ZTEST(mpipe_ipc_ring, test_a_consumer_waits_for_a_producer)
{
	struct mpipe_ipc_ring consumer;

	reset_region();
	zassert_equal(mpipe_ipc_ring_init(&consumer, shared(), &ops, false, PERIOD_BYTES,
					  PERIOD_COUNT, REGION_SIZE),
		      -EAGAIN, "no geometry published yet means no stream yet");
}

/* A consumer that arrives late joins the stream rather than replaying it. */
ZTEST(mpipe_ipc_ring, test_a_late_consumer_joins_where_the_producer_is)
{
	struct mpipe_ipc_ring producer, consumer;

	reset_region();
	zassert_ok(mpipe_ipc_ring_init(&producer, shared(), &ops, true, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));

	produce_tagged(&producer, 0x11);
	produce_tagged(&producer, 0x22);

	zassert_ok(mpipe_ipc_ring_init(&consumer, shared(), &ops, false, PERIOD_BYTES,
				       PERIOD_COUNT, REGION_SIZE));
	zassert_equal(mpipe_ipc_ring_fill(&consumer), 0U,
		      "stale periods from before the consumer existed are not audio");

	produce_tagged(&producer, 0x33);
	consume_expecting(&consumer, 0x33);
}

ZTEST(mpipe_ipc_ring, test_geometry_must_fit_the_region)
{
	struct mpipe_ipc_ring producer;

	reset_region();
	zassert_equal(mpipe_ipc_ring_capacity(REGION_SIZE, PERIOD_BYTES), PERIOD_COUNT);
	zassert_equal(mpipe_ipc_ring_capacity(MPIPE_IPC_RING_DATA_OFFSET, PERIOD_BYTES),
		      0U, "a region with no room for data holds no periods");

	zassert_equal(mpipe_ipc_ring_init(&producer, shared(), &ops, true, PERIOD_BYTES,
					  PERIOD_COUNT + 1U, REGION_SIZE),
		      -ENOSPC);
	zassert_equal(mpipe_ipc_ring_init(&producer, shared(), &ops, true, 0U,
					  PERIOD_COUNT, REGION_SIZE),
		      -EINVAL);
	zassert_equal(mpipe_ipc_ring_init(NULL, shared(), &ops, true, PERIOD_BYTES,
					  PERIOD_COUNT, REGION_SIZE),
		      -EINVAL);
}

ZTEST(mpipe_ipc_ring, test_roles_are_enforced)
{
	struct mpipe_ipc_ring producer, consumer;

	open_pair(&producer, &consumer);

	zassert_is_null(mpipe_ipc_ring_claim_read(&producer),
			"a producer must not consume");
	zassert_is_null(mpipe_ipc_ring_claim_write(&consumer),
			"a consumer must not produce");
	zassert_equal(mpipe_ipc_ring_commit_write(&consumer), -EINVAL);
	zassert_equal(mpipe_ipc_ring_commit_read(&producer), -EINVAL);

	/* A second claim without a commit is a caller error, not a new slot. */
	zassert_not_null(mpipe_ipc_ring_claim_write(&producer));
	zassert_is_null(mpipe_ipc_ring_claim_write(&producer));
}
