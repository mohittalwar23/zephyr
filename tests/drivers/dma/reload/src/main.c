/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>
#include <dma_nxp_sdma_accounting.h>
#include <dma_nxp_sdma_lifecycle.h>

#define N  4
#define SZ 32U
#define CH 0U
#define LIFECYCLE_STACK_SIZE 512

#if DT_NODE_EXISTS(DT_NODELABEL(tst_dma0))
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(tst_dma0));
static K_SEM_DEFINE(done, 0, 1);
#endif

#if defined(CONFIG_SMP)
static K_SEM_DEFINE(restart_entered, 0, 1);
static K_SEM_DEFINE(completion_done, 0, 1);
static K_THREAD_STACK_DEFINE(completion_stack, LIFECYCLE_STACK_SIZE);
static struct k_thread completion_thread_data;
static struct dma_nxp_sdma_lifecycle lifecycle;
static bool hardware_started;
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(tst_dma0))
static int cbs;
static __aligned(32) char src[N][SZ];
static __aligned(32) char dst[N][SZ];

static void on_done(const struct device *d, void *a, uint32_t id, int status)
{
	cbs++;
	k_sem_give(&done);
}

static struct dma_config base_cfg(struct dma_block_config *blk)
{
	struct dma_config c = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1U,
		.dest_data_size = 1U,
		.source_burst_length = 1U,
		.dest_burst_length = 1U,
		.block_count = 1U,
		.head_block = blk,
		.dma_callback = on_done,
		.user_data = &done,
	};
	return c;
}

/* A channel must deliver a completion for every reload, moving each new block. */
ZTEST(dma_reload, test_reload_completes_each_time)
{
	struct dma_block_config blk = {
		.source_address = (uintptr_t)src[0],
		.dest_address = (uintptr_t)dst[0],
		.block_size = SZ,
	};
	struct dma_config c = base_cfg(&blk);

	cbs = 0;
	k_sem_reset(&done);
	memset(src[0], 'A', SZ);
	zassert_ok(dma_config(dma, CH, &c));
	zassert_ok(dma_start(dma, CH));
	zassert_ok(k_sem_take(&done, K_MSEC(1000)));

	for (int i = 1; i < N; i++) {
		memset(src[i], 'A' + i, SZ);
		k_sem_reset(&done);
		zassert_ok(dma_reload(dma, CH, (uintptr_t)src[i], (uintptr_t)dst[i], SZ),
			   "reload %d failed", i);
		zassert_ok(k_sem_take(&done, K_MSEC(1000)), "no completion for reload %d", i);
		zassert_mem_equal(dst[i], src[i], SZ, "reload %d moved wrong data", i);
	}
	zassert_equal(cbs, N, "expected %d completions, got %d", N, cbs);
}

/* dma_get_status() must report a defined struct, not leave the caller's stack. */
ZTEST(dma_reload, test_get_status_conformance)
{
	struct dma_block_config blk = {
		.source_address = (uintptr_t)src[0],
		.dest_address = (uintptr_t)dst[0],
		.block_size = SZ,
	};
	struct dma_config c = base_cfg(&blk);
	struct dma_status st;

	zassert_ok(dma_config(dma, CH, &c));
	zassert_ok(dma_get_status(dma, CH, &st), "get_status must be implemented");
	zassert_equal(st.dir, MEMORY_TO_MEMORY, "dir not reported");
}

ZTEST(dma_reload, test_get_status_after_stop)
{
	struct dma_block_config blk = {
		.source_address = (uintptr_t)src[0],
		.dest_address = (uintptr_t)dst[0],
		.block_size = SZ,
	};
	struct dma_config c = base_cfg(&blk);
	struct dma_status st = {
		.busy = true,
		.dir = PERIPHERAL_TO_MEMORY,
		.pending_length = UINT32_MAX,
		.free = UINT32_MAX,
		.write_position = UINT32_MAX,
		.read_position = UINT32_MAX,
		.total_copied = UINT64_MAX,
	};

	zassert_ok(dma_config(dma, CH, &c));
	zassert_ok(dma_stop(dma, CH));
	zassert_ok(dma_get_status(dma, CH, &st));
	zassert_false(st.busy, "channel is still busy after stop");
	zassert_equal(st.pending_length, 0U, "pending length was not initialized");
	zassert_equal(st.free, 0U, "free space was not initialized");
	zassert_equal(st.dir, MEMORY_TO_MEMORY, "direction was not initialized");
	zassert_equal(st.read_position, 0U, "read position was not initialized");
	zassert_equal(st.write_position, 0U, "write position was not initialized");
	zassert_equal(st.total_copied, 0U, "total copied was not initialized");
}

struct sdma_mock_descriptor {
	uintptr_t source_address;
	uintptr_t dest_address;
	uint32_t count;
};

struct sdma_mock_ring {
	struct sdma_mock_descriptor descriptor[DMA_NXP_SDMA_BD_COUNT];
	uint32_t rearmed_index;
};

static void sdma_mock_rearm(void *context, uint32_t index, uint32_t size)
{
	struct sdma_mock_ring *ring = context;

	ring->descriptor[index].count = size;
	ring->rearmed_index = index;
}

static void test_sdma_cyclic_descriptor_accounting(uint32_t direction)
{
	struct dma_block_config blocks[] = {
		{
			.source_address = 0x1000U,
			.dest_address = 0x2000U,
			.block_size = 16U,
		},
		{
			.source_address = 0x3000U,
			.dest_address = 0x4000U,
			.block_size = 24U,
		},
	};
	struct dma_nxp_sdma_descriptor_state state;
	struct dma_config config = base_cfg(&blocks[0]);
	struct sdma_mock_ring ring;
	const uint32_t next_bd[] = { 1U, 0U, 1U };
	const uint32_t expected_bd[] = { 0U, 1U, 0U };
	const uint32_t expected_size[] = { 16U, 24U, 16U };
	const uint64_t expected_total[] = { 16U, 40U, 56U };
	uintptr_t source_address[DMA_NXP_SDMA_BD_COUNT];
	uintptr_t dest_address[DMA_NXP_SDMA_BD_COUNT];

	blocks[0].next_block = &blocks[1];
	config.block_count = ARRAY_SIZE(blocks);
	config.channel_direction = direction;
	zassert_ok(dma_nxp_sdma_descriptor_prepare(&state, &config));
	zassert_ok(dma_nxp_sdma_descriptor_init_stat(&state, direction));
	for (size_t i = 0; i < DMA_NXP_SDMA_BD_COUNT; i++) {
		ring.descriptor[i].source_address = state.source_address[i];
		ring.descriptor[i].dest_address = state.dest_address[i];
		ring.descriptor[i].count = 0U;
		source_address[i] = ring.descriptor[i].source_address;
		dest_address[i] = ring.descriptor[i].dest_address;
	}

	for (size_t i = 0; i < ARRAY_SIZE(next_bd); i++) {
		zassert_ok(dma_nxp_sdma_descriptor_complete(&state, direction, next_bd[i],
						      sdma_mock_rearm, &ring));
		zassert_equal(ring.rearmed_index, expected_bd[i],
			      "completion %zu rearmed wrong BD", i);
		zassert_equal(ring.descriptor[expected_bd[i]].count, expected_size[i],
			      "completion %zu rearmed wrong size", i);
		zassert_equal(state.stat.total_copied, expected_total[i],
			      "completion %zu accounted wrong total", i);
		zassert_ok(dma_nxp_sdma_descriptor_reload(&state, direction, 0U, 0U,
						   expected_size[i]));
		zassert_equal(state.stat.total_copied, expected_total[i],
			      "reload %zu was counted as hardware progress", i);
	}

	for (size_t i = 0; i < DMA_NXP_SDMA_BD_COUNT; i++) {
		zassert_equal(ring.descriptor[i].source_address, source_address[i],
			      "reload changed source address %zu", i);
		zassert_equal(ring.descriptor[i].dest_address, dest_address[i],
			      "reload changed destination address %zu", i);
	}
}

ZTEST(dma_reload, test_sdma_cyclic_descriptor_accounting)
{
	test_sdma_cyclic_descriptor_accounting(PERIPHERAL_TO_MEMORY);
	test_sdma_cyclic_descriptor_accounting(MEMORY_TO_PERIPHERAL);
}

ZTEST(dma_reload, test_sdma_descriptor_prepare_validates_ring)
{
	struct dma_block_config blocks[] = {
		{
			.source_address = 0x1000U,
			.dest_address = 0x2000U,
			.block_size = UINT16_MAX,
		},
		{
			.source_address = 0x3000U,
			.dest_address = 0x4000U,
			.block_size = UINT16_MAX,
		},
	};
	struct dma_nxp_sdma_descriptor_state state;
	struct dma_config config = base_cfg(&blocks[0]);
	struct sdma_mock_ring ring;

	blocks[0].next_block = &blocks[1];
	config.block_count = ARRAY_SIZE(blocks);
	config.channel_direction = PERIPHERAL_TO_MEMORY;
	zassert_ok(dma_nxp_sdma_descriptor_prepare(&state, &config));
	zassert_equal(state.capacity, 2U * UINT16_MAX, "largest ring capacity was wrong");
	zassert_equal(dma_nxp_sdma_descriptor_complete(&state, MEMORY_TO_MEMORY, 1U,
			      sdma_mock_rearm, &ring), -EINVAL,
		      "completion accepted an unsupported direction");
	zassert_equal(dma_nxp_sdma_descriptor_reload(&state, MEMORY_TO_MEMORY, 0U, 0U,
			      UINT16_MAX), -EINVAL, "reload accepted an unsupported direction");

	config.block_count = 0U;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "zero descriptor count was accepted");

	config.block_count = DMA_NXP_SDMA_BD_COUNT + 1U;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "oversized descriptor count was accepted");

	config.block_count = ARRAY_SIZE(blocks);
	blocks[0].next_block = NULL;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "short descriptor chain was accepted");

	blocks[0].next_block = &blocks[1];

	/*
	 * A cyclic list is circular: the last block links back to the head.
	 * Consumers build their rings this way, so it must be accepted.
	 */
	blocks[1].next_block = &blocks[0];
	zassert_ok(dma_nxp_sdma_descriptor_prepare(&state, &config),
		   "circular block list was rejected");
	zassert_equal(state.capacity, 2U * UINT16_MAX, "circular ring capacity was wrong");
	blocks[1].next_block = NULL;

	blocks[1].block_size = 0U;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "zero descriptor size was accepted");

	blocks[1].block_size = UINT16_MAX + 1U;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "unrepresentable descriptor size was accepted");

	blocks[1].block_size = UINT16_MAX;
	config.channel_direction = MEMORY_TO_MEMORY;
	zassert_equal(dma_nxp_sdma_descriptor_prepare(&state, &config), -EINVAL,
		      "unsupported direction was accepted");
}
#endif

#if defined(CONFIG_SMP)
static bool complete_transfer(void *context)
{
	ARG_UNUSED(context);
	return true;
}

static void restart_hardware(void *context)
{
	ARG_UNUSED(context);
	k_sem_give(&restart_entered);
	k_busy_wait(50000);
	hardware_started = true;
}

static void stop_hardware(void *context)
{
	ARG_UNUSED(context);
	hardware_started = false;
}

static void complete_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	dma_nxp_sdma_lifecycle_complete(&lifecycle, complete_transfer, NULL,
					restart_hardware, NULL);
	k_sem_give(&completion_done);
}

ZTEST(dma_reload, test_stop_serializes_internal_restart)
{
	lifecycle.started = true;
	hardware_started = false;
	k_sem_reset(&restart_entered);
	k_sem_reset(&completion_done);
	k_thread_create(&completion_thread_data, completion_stack,
			K_THREAD_STACK_SIZEOF(completion_stack), complete_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_FOREVER);
	zassert_ok(k_thread_cpu_pin(&completion_thread_data, 1));
	k_thread_start(&completion_thread_data);
	zassert_ok(k_sem_take(&restart_entered, K_MSEC(1000)));
	dma_nxp_sdma_lifecycle_stop(&lifecycle, stop_hardware, NULL);
	zassert_ok(k_sem_take(&completion_done, K_MSEC(1000)));
	zassert_false(lifecycle.started, "channel must remain stopped");
	zassert_false(hardware_started, "stop must be the final hardware operation");
}
#endif

static void *setup(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(tst_dma0))
	zassert_true(device_is_ready(dma), "DMA controller not ready");
#endif
	return NULL;
}

/* Stop after each test so the next one can (re)configure a fresh channel.
 * This runs only once a channel has been configured, so it never stops an
 * unconfigured channel.
 */
static void after(void *fixture)
{
	ARG_UNUSED(fixture);
#if DT_NODE_EXISTS(DT_NODELABEL(tst_dma0))
	(void)dma_stop(dma, CH);
#endif
}

ZTEST_SUITE(dma_reload, NULL, setup, NULL, after, NULL);
