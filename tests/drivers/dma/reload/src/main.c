/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#define N  4
#define SZ 32U
#define CH 0U

static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(tst_dma0));
static K_SEM_DEFINE(done, 0, 1);
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

static void *setup(void)
{
	zassert_true(device_is_ready(dma), "DMA controller not ready");
	return NULL;
}

/* Stop after each test so the next one can (re)configure a fresh channel.
 * This runs only once a channel has been configured, so it never stops an
 * unconfigured channel.
 */
static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)dma_stop(dma, CH);
}

ZTEST_SUITE(dma_reload, NULL, setup, NULL, after, NULL);
