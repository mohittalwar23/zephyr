/* Copyright 2026 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_MPIPE_AUD_I2S_CODEC_SINK_INTERNAL_H_
#define ZEPHYR_SUBSYS_MPIPE_AUD_I2S_CODEC_SINK_INTERNAL_H_

#include <errno.h>
#include <stdint.h>

static inline int mpipe_aud_i2s_codec_sink_validate_prime(uint8_t count,
						   uint8_t max_count)
{
	if (count < 2U) {
		return -EINVAL;
	}
	if (max_count != 0U && count > max_count) {
		return -ENOBUFS;
	}
	return 0;
}

#endif /* ZEPHYR_SUBSYS_MPIPE_AUD_I2S_CODEC_SINK_INTERNAL_H_ */
