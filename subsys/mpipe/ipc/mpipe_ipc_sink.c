/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/mpipe/aud/mpipe_aud.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_sink.h>
#include <zephyr/mpipe/mpipe_buffer.h>

LOG_MODULE_REGISTER(mpipe_ipc_sink, CONFIG_MPIPE_LOG_LEVEL);

/*
 * Take one channel and narrow it to 16 bits.
 *
 * The peer's model is 16-bit mono, and pipelines on this board are commonly
 * stereo and 32-bit wide, so the conversion belongs here rather than in the
 * ring: the ring carries whatever the two cores agreed on, and this is the
 * element that knows what arrived.
 */
static void narrow_to_mono16(int16_t *dst, const uint8_t *src, uint32_t frames,
			     uint32_t channels, uint32_t sample_bytes)
{
	for (uint32_t i = 0; i < frames; i++) {
		const uint8_t *frame = src + ((size_t)i * channels * sample_bytes);

		if (sample_bytes == sizeof(int16_t)) {
			int16_t sample;

			memcpy(&sample, frame, sizeof(sample));
			dst[i] = sample;
		} else {
			int32_t sample;

			memcpy(&sample, frame, sizeof(sample));
			/*
			 * A 32-bit frame carries its signal in the upper bits
			 * on this hardware, so the top half is the 16-bit
			 * sample rather than a truncation of the low half.
			 */
			dst[i] = (int16_t)(sample >> 16);
		}
	}
}

static int ipc_sink_set_caps(struct mpipe_sink *sink, const struct mpipe_structure *caps)
{
	struct mpipe_ipc_sink *ipc_sink = (struct mpipe_ipc_sink *)sink;
	uint32_t sample_rate, bit_width, channels, frame_interval;
	uint32_t frames;

	if (ipc_sink->ring == NULL) {
		LOG_ERR("no ring to produce into");
		return -EINVAL;
	}

	if (mpipe_aud_caps_get_uint(caps, MPIPE_CAPS_SAMPLE_RATE, &sample_rate) != 0 ||
	    mpipe_aud_caps_get_uint(caps, MPIPE_CAPS_BITWIDTH, &bit_width) != 0 ||
	    mpipe_aud_caps_get_uint(caps, MPIPE_CAPS_NUM_OF_CHANNEL, &channels) != 0 ||
	    mpipe_aud_caps_get_uint(caps, MPIPE_CAPS_FRAME_INTERVAL, &frame_interval) != 0) {
		return -EINVAL;
	}

	if (channels == 0U || (bit_width % 8U) != 0U) {
		return -EINVAL;
	}

	/*
	 * The peer sized its periods before this pipeline existed, so the
	 * negotiated format has to land on the same number of frames. Checking
	 * it here turns a silent half-filled period into a refusal at link
	 * time, which is the same reason the ring checks its own geometry.
	 */
	ipc_sink->period_samples = ipc_sink->ring->period_bytes / sizeof(int16_t);
	frames = (sample_rate / 1000U) * (frame_interval / 1000U);

	if (frames != ipc_sink->period_samples) {
		LOG_ERR("%u frames per buffer, but the peer expects %u", frames,
			ipc_sink->period_samples);
		return -EINVAL;
	}

	ipc_sink->channels = channels;
	ipc_sink->sample_bytes = bit_width / 8U;
	ipc_sink->configured = true;

	LOG_INF("forwarding %u Hz, %u ch, %u-bit as %u-sample mono periods", sample_rate,
		channels, bit_width, ipc_sink->period_samples);

	return 0;
}

static int ipc_sink_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
			     struct net_buf **out_buf)
{
	struct mpipe_ipc_sink *ipc_sink;
	uint32_t bytes_used;
	uint32_t frames;
	int16_t *period;

	__ASSERT_NO_MSG(pad != NULL);
	__ASSERT_NO_MSG(in_buf != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	ipc_sink = CONTAINER_OF(pad->object.container, struct mpipe_ipc_sink,
				sink.element.object);

	/* A sink is the end of the chain. */
	*out_buf = NULL;

	if (!ipc_sink->configured) {
		net_buf_unref(in_buf);
		return -EINVAL;
	}

	bytes_used = mpipe_buffer_get_meta(in_buf)->bytes_used;
	frames = bytes_used / (ipc_sink->channels * ipc_sink->sample_bytes);

	period = mpipe_ipc_ring_claim_write(ipc_sink->ring);
	if (period == NULL) {
		/*
		 * The peer is behind. Drop this period rather than stall the
		 * capture that feeds it: the branch that makes the audio
		 * audible shares this pipeline, and blocking here would stop
		 * that too.
		 */
		mpipe_ipc_ring_record_overrun(ipc_sink->ring);
		ipc_sink->dropped++;
		net_buf_unref(in_buf);
		return 0;
	}

	narrow_to_mono16(period, in_buf->data, MIN(frames, ipc_sink->period_samples),
			 ipc_sink->channels, ipc_sink->sample_bytes);

	(void)mpipe_ipc_ring_commit_write(ipc_sink->ring);

	net_buf_unref(in_buf);

	return 0;
}

int mpipe_ipc_sink_init(struct mpipe_ipc_sink *ipc_sink, uint8_t id,
			struct mpipe_ipc_ring *ring)
{
	int ret;

	if (ipc_sink == NULL || ring == NULL) {
		return -EINVAL;
	}

	memset(ipc_sink, 0, sizeof(*ipc_sink));

	ret = mpipe_sink_init(&ipc_sink->sink, id);
	if (ret < 0) {
		return ret;
	}

	ipc_sink->ring = ring;
	ipc_sink->sink.sink_pad.chain_fn = ipc_sink_chain_fn;
	ipc_sink->sink.set_caps = ipc_sink_set_caps;

	return 0;
}
