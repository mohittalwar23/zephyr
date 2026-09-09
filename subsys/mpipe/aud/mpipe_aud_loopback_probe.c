/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/mpipe/mpipe_latency.h>
#include <zephyr/mpipe/aud/mpipe_aud_loopback_probe.h>

LOG_MODULE_REGISTER(mpipe_aud_loopback, CONFIG_MPIPE_LOG_LEVEL);

enum probe_state {
	PROBE_IDLE = 0,
	/*
	 * Play silence first and watch what still comes back. A loopback whose
	 * microphone can hear its own output feeds back and saturates, and a
	 * detector armed on amplitude alone would then trigger on the first
	 * buffer it saw and report one buffer period as if it were a round
	 * trip. Quieting the output first lets that die away and measures the
	 * floor the burst has to beat.
	 */
	PROBE_QUIET,
	PROBE_ARMED,
	PROBE_PENDING,
	PROBE_DONE,
};

/*
 * Buffers of silence before the burst; at a 10 ms frame that is one second.
 * It has to outlast the decay of any feedback the graph was sustaining before
 * the probe silenced it, because a burst emitted while the room is still
 * ringing is detected against that ringing rather than against quiet. Three
 * hundred milliseconds was not enough: measurements alternated between a clean
 * result and none at all, depending on whether the previous run had left the
 * loop howling.
 */
#define PROBE_QUIET_BUFFERS 100U
#define PROBE_FLOOR_BUFFERS 10U
#define PROBE_PENDING_BUFFERS 100U

static struct k_spinlock lock;
static enum probe_state state;
static uint8_t width_bits = 16U;
static uint32_t threshold;
static uint32_t emit_cyc;
static uint32_t result_us;
static uint32_t result_peak;
static uint32_t result_index;
static uint32_t result_total;
static uint32_t noise_floor;
static uint32_t quiet_left;
static uint32_t quiet_floor;
static uint32_t pending_left;
static bool result_valid;
static bool result_timed_out;

int mpipe_aud_loopback_arm(uint8_t bit_width, uint32_t thresh)
{
	if (bit_width != 16U && bit_width != 32U) {
		return -EINVAL;
	}

	/*
	 * A threshold fixed against full scale is wrong for an acoustic path:
	 * the burst leaves the DAC at full scale but arrives at the microphone
	 * many orders of magnitude down, so half of full scale is unreachable
	 * and nothing is ever detected. Zero means "decide it from the floor
	 * measured while the output is silent", which is what adapts to
	 * whatever coupling actually exists.
	 */

	K_SPINLOCK(&lock) {
		width_bits = bit_width;
		threshold = thresh;
		noise_floor = 0U;
		quiet_floor = 0U;
		quiet_left = PROBE_QUIET_BUFFERS;
		pending_left = PROBE_PENDING_BUFFERS;
		result_us = 0U;
		result_peak = 0U;
		result_index = 0U;
		result_total = 0U;
		result_valid = false;
		result_timed_out = false;
		state = PROBE_QUIET;
	}

	return 0;
}

void mpipe_aud_loopback_get(struct mpipe_aud_loopback_result *out)
{
	if (out == NULL) {
		return;
	}

	K_SPINLOCK(&lock) {
		out->valid = result_valid;
		out->timed_out = result_timed_out;
		out->pending = (state == PROBE_PENDING);
		out->round_trip_us = result_us;
		out->peak = result_peak;
		out->noise_floor = noise_floor;
		out->quiet_floor = quiet_floor;
		out->hit_index = result_index;
		out->hit_total = result_total;
	}
}

void mpipe_aud_loopback_emit(void *data, size_t len)
{
	bool fill = false;
	bool silence = false;
	uint8_t bits = 0U;

	if (data == NULL || len == 0U) {
		return;
	}

	K_SPINLOCK(&lock) {
		/*
		 * Keep the output silent for the whole measurement, not just
		 * before the burst. This graph plays its own input back, so
		 * with the microphone and the earphone acoustically coupled it
		 * howls, and the detector then triggers on feedback rather than
		 * on the burst - which reads as a plausible round trip one time
		 * and as several hundred milliseconds the next. Silence
		 * everything except the single burst buffer and the only thing
		 * that can come back is the burst.
		 */
		if (state == PROBE_PENDING) {
			if (pending_left > 0U) {
				pending_left--;
			}
			if (pending_left == 0U) {
				result_valid = false;
				result_timed_out = true;
				state = PROBE_DONE;
			} else {
				silence = true;
			}
		} else if (state == PROBE_QUIET) {
			silence = true;
			if (quiet_left > 0U) {
				quiet_left--;
			}
			if (quiet_left == 0U) {
				if (threshold == 0U) {
					/*
					 * Four times what still arrives with the
					 * output silent: comfortably clear of the
					 * ambient level without assuming how much
					 * of the burst survives the trip.
					 */
					uint32_t floor = MAX(quiet_floor, 1U);

					threshold = (floor > (UINT32_MAX / 4U))
							    ? UINT32_MAX
							    : (floor * 4U);
				}
				state = PROBE_ARMED;
			}
		} else if (state == PROBE_ARMED) {
			fill = true;
			bits = width_bits;
			state = PROBE_PENDING;
		}
	}

	if (silence) {
		memset(data, 0, len);
		return;
	}

	if (!fill) {
		return;
	}

	/*
	 * A square burst rather than a single sample: one sample would be
	 * filtered away by the codec's own reconstruction and anti-alias
	 * filters long before it reached the microphone, while a burst holding
	 * full scale for the whole buffer survives both and still has an edge
	 * sharp enough to time against, because what is timed is the first
	 * buffer it appears in.
	 */
	if (bits == 16U) {
		int16_t *p = data;

		for (size_t i = 0; i < len / sizeof(*p); i++) {
			p[i] = (i & 1U) ? INT16_MIN : INT16_MAX;
		}
	} else {
		int32_t *p = data;

		for (size_t i = 0; i < len / sizeof(*p); i++) {
			p[i] = (i & 1U) ? INT32_MIN : INT32_MAX;
		}
	}

	/* Stamp as late as possible, so the buffer really is on its way out. */
	K_SPINLOCK(&lock) {
		emit_cyc = k_cycle_get_32();
	}
}

void mpipe_aud_loopback_detect(const void *data, size_t len)
{
	uint32_t peak = 0U;
	uint32_t first_over = 0U;
	uint32_t total = 0U;
	bool found = false;
	uint8_t bits;
	uint32_t thresh;
	enum probe_state state_snapshot;

	if (data == NULL || len == 0U) {
		return;
	}

	K_SPINLOCK(&lock) {
		bits = width_bits;
		thresh = threshold;
		state_snapshot = state;
	}

	if (state_snapshot != PROBE_QUIET && state_snapshot != PROBE_PENDING) {
		return;
	}

	if (bits == 16U) {
		const int16_t *p = data;

		total = len / sizeof(*p);
		for (uint32_t i = 0; i < total; i++) {
			uint32_t mag = (p[i] < 0) ? (uint32_t)(-(int32_t)p[i]) : (uint32_t)p[i];

			peak = MAX(peak, mag);
			if (!found && mag >= thresh) {
				first_over = i;
				found = true;
			}
		}
	} else {
		const int32_t *p = data;

		total = len / sizeof(*p);
		for (uint32_t i = 0; i < total; i++) {
			uint32_t mag = (p[i] < 0) ? (uint32_t)(-(int64_t)p[i]) : (uint32_t)p[i];

			peak = MAX(peak, mag);
			if (!found && mag >= thresh) {
				first_over = i;
				found = true;
			}
		}
	}

	K_SPINLOCK(&lock) {
		noise_floor = MAX(noise_floor, peak);

		/*
		 * The first quiet buffers still contain audio that was already in
		 * the converters and acoustic feedback path when muting began.  A
		 * maximum over that settling transient can stay at full scale and
		 * make the adaptive threshold unreachable.  Measure the floor only
		 * over the settled tail of the quiet interval.
		 */
		if (state == PROBE_QUIET && quiet_left <= PROBE_FLOOR_BUFFERS) {
			quiet_floor = MAX(quiet_floor, peak);
		}

		/*
		 * Beat both the absolute threshold and what was still arriving
		 * with the output silent, so a saturated or noisy input cannot
		 * pass for the burst.
		 */
		if (state == PROBE_PENDING && peak >= thresh &&
		    (quiet_floor < (UINT32_MAX / 2U)) && peak >= (quiet_floor * 2U)) {
			result_us = mpipe_latency_cyc_delta_to_us(emit_cyc, k_cycle_get_32());
			result_peak = peak;
			result_index = first_over;
			result_total = total;
			result_valid = true;
			result_timed_out = false;
			state = PROBE_DONE;
		}
	}
}
