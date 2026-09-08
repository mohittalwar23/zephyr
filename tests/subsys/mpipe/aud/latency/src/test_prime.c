/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "mpipe_aud_i2s_codec_sink_internal.h"

ZTEST(mpipe_aud_latency, test_silence_prime_bounds)
{
	zassert_ok(mpipe_aud_i2s_codec_sink_validate_prime(2U, 3U));
	zassert_ok(mpipe_aud_i2s_codec_sink_validate_prime(3U, 3U));
	zassert_ok(mpipe_aud_i2s_codec_sink_validate_prime(3U, 0U));
	zassert_equal(mpipe_aud_i2s_codec_sink_validate_prime(1U, 3U), -EINVAL,
		      "known-starving prime was accepted");
	zassert_equal(mpipe_aud_i2s_codec_sink_validate_prime(4U, 3U), -ENOBUFS,
		      "prime above queue capacity was accepted");
}
