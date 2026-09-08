/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <zephyr/mpipe/aud/mpipe_aud_loopback_probe.h>

#define QUIET_CALLS 100U

#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)

ZTEST(mpipe_aud_latency, test_probe_rejects_invalid_width)
{
	zassert_equal(mpipe_aud_loopback_arm(8U, 4U), -EINVAL,
		      "8-bit probe width was accepted");
	zassert_equal(mpipe_aud_loopback_arm(24U, 4U), -EINVAL,
		      "24-bit probe width was accepted");
}

ZTEST(mpipe_aud_latency, test_probe_detects_returned_burst)
{
	int16_t buffer[32] = {0};
	struct mpipe_aud_loopback_result result;
	struct mpipe_aud_loopback_result after_done;

	zassert_ok(mpipe_aud_loopback_arm(16U, 4U));
	for (uint32_t i = 0U; i < QUIET_CALLS; i++) {
		mpipe_aud_loopback_emit(buffer, sizeof(buffer));
	}
	mpipe_aud_loopback_emit(buffer, sizeof(buffer));
	mpipe_aud_loopback_detect(buffer, sizeof(buffer));
	mpipe_aud_loopback_get(&result);

	zassert_true(result.valid, "returned burst was not detected");
	zassert_false(result.timed_out, "successful probe reported timeout");
	zassert_false(result.pending, "successful probe remained pending");
	zassert_equal(result.hit_index, 0U, "wrong first matching sample");

	memset(buffer, 0x7f, sizeof(buffer));
	mpipe_aud_loopback_detect(buffer, sizeof(buffer));
	mpipe_aud_loopback_get(&after_done);
	zassert_equal(after_done.noise_floor, result.noise_floor,
		      "DONE state kept scanning capture");

	zassert_ok(mpipe_aud_loopback_arm(16U, 4U));
	mpipe_aud_loopback_get(&after_done);
	zassert_false(after_done.valid, "rearm retained successful result");
	zassert_false(after_done.timed_out, "rearm invented timeout");
}

ZTEST(mpipe_aud_latency, test_probe_timeout_restores_audio)
{
	int16_t buffer[32];
	int16_t expected[32];
	struct mpipe_aud_loopback_result result;

	zassert_ok(mpipe_aud_loopback_arm(16U, UINT16_MAX));
	memset(buffer, 0x2a, sizeof(buffer));
	for (uint32_t i = 0U; i < QUIET_CALLS + 1U; i++) {
		mpipe_aud_loopback_emit(buffer, sizeof(buffer));
	}
	for (uint32_t i = 0U; i < 256U; i++) {
		memset(buffer, 0x2a, sizeof(buffer));
		mpipe_aud_loopback_emit(buffer, sizeof(buffer));
		mpipe_aud_loopback_get(&result);
		if (result.timed_out) {
			break;
		}
	}

	zassert_true(result.timed_out, "missing return never timed out");
	zassert_false(result.valid, "timeout reported a valid measurement");
	zassert_false(result.pending, "timeout remained pending");

	memset(buffer, 0x2a, sizeof(buffer));
	memcpy(expected, buffer, sizeof(buffer));
	mpipe_aud_loopback_emit(buffer, sizeof(buffer));
	zassert_mem_equal(buffer, expected, sizeof(buffer),
			  "probe kept muting after timeout");

	zassert_ok(mpipe_aud_loopback_arm(16U, 4U));
	mpipe_aud_loopback_get(&result);
	zassert_false(result.valid, "rearm retained timeout as success");
	zassert_false(result.timed_out, "rearm retained timeout result");
}

#else

ZTEST(mpipe_aud_latency, test_disabled_probe_stubs_are_inert)
{
	int16_t buffer[4] = {1, 2, 3, 4};
	const int16_t expected[4] = {1, 2, 3, 4};
	struct mpipe_aud_loopback_result result = {.valid = true, .timed_out = true};

	zassert_equal(mpipe_aud_loopback_arm(16U, 4U), -ENOSYS,
		      "disabled arm did not report unsupported operation");
	mpipe_aud_loopback_emit(buffer, sizeof(buffer));
	mpipe_aud_loopback_detect(buffer, sizeof(buffer));
	mpipe_aud_loopback_get(&result);
	zassert_mem_equal(buffer, expected, sizeof(buffer),
			  "disabled probe modified audio");
	zassert_false(result.valid, "disabled get retained a valid result");
	zassert_false(result.timed_out, "disabled get retained a timeout");
}

#endif
