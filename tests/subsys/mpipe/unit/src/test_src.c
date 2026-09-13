/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/mpipe/mpipe_src.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(mpipe_src_api, NULL, NULL, NULL, NULL, NULL);

/* Initialization is a constructor, not a request for callers to zero storage. */
ZTEST(mpipe_src_api, test_init_overwrites_nonzero_scheduling_state)
{
	struct mpipe_src src;

	memset(&src, 0xa5, sizeof(src));
	zassert_ok(mpipe_src_init(&src, 1U));

	zassert_equal(src.drive, MPIPE_SRC_DRIVE_PULL);
	zassert_is_null(src.pool);
	zassert_equal(src.num_buffers, 0U);
	zassert_is_null(src.decide_buffer_pool);
}
