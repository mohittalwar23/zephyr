/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>

/* Audio format field offsets, from the start of a CONFIG payload. */
#define FMT_OFF_RATE      0U
#define FMT_OFF_SAMPLES   4U
#define FMT_OFF_CHANNELS  6U
#define FMT_OFF_FORMAT    7U
#define FMT_OFF_LAYOUT    8U
#define FMT_OFF_DURATION  9U
#define FMT_OFF_POLICY    10U
#define FMT_OFF_RESERVED  11U
#define FMT_LENGTH        12U

/* Policy bits inside FMT_OFF_POLICY. */
#define FMT_POLICY_UNDERRUN_PERMITTED BIT(0)
#define FMT_POLICY_OVERRUN_PERMITTED  BIT(1)

static bool type_is_known(uint16_t type)
{
	switch (type) {
	case MPIPE_IPC_TYPE_HELLO:
	case MPIPE_IPC_TYPE_HELLO_ACK:
	case MPIPE_IPC_TYPE_CONFIG:
	case MPIPE_IPC_TYPE_CONFIG_ACK:
	case MPIPE_IPC_TYPE_START:
	case MPIPE_IPC_TYPE_START_ACK:
	case MPIPE_IPC_TYPE_STOP:
	case MPIPE_IPC_TYPE_STOP_ACK:
	case MPIPE_IPC_TYPE_STATUS:
	case MPIPE_IPC_TYPE_ERROR:
	case MPIPE_IPC_TYPE_HEARTBEAT:
	case MPIPE_IPC_TYPE_HEARTBEAT_ACK:
		return true;
	default:
		return false;
	}
}

int mpipe_ipc_encode(void *dst, size_t capacity, const struct mpipe_ipc_message *message,
		     size_t *written)
{
	uint8_t *buf = dst;
	size_t total;

	if (dst == NULL || message == NULL || written == NULL) {
		return -EINVAL;
	}
	if (message->payload_length != 0U && message->payload == NULL) {
		return -EINVAL;
	}
	if (message->payload_length > MPIPE_IPC_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	total = MPIPE_IPC_HEADER_LENGTH + message->payload_length;
	if (capacity < total) {
		return -ENOSPC;
	}

	sys_put_le32((uint32_t)total, &buf[MPIPE_IPC_OFF_SIZE]);
	sys_put_le32(message->header.cmd, &buf[MPIPE_IPC_OFF_CMD]);
	sys_put_le32(message->header.generation, &buf[MPIPE_IPC_OFF_GENERATION]);

	if (message->payload_length != 0U) {
		memcpy(&buf[MPIPE_IPC_HEADER_LENGTH], message->payload,
		       message->payload_length);
	}

	*written = total;

	return 0;
}

int mpipe_ipc_decode(struct mpipe_ipc_message *message, const void *src, size_t length)
{
	const uint8_t *buf = src;
	struct mpipe_ipc_header header;
	size_t payload_length;

	if (message == NULL || src == NULL) {
		return -EINVAL;
	}

	/* Nothing is exposed until every rule below has passed. */
	message->payload = NULL;
	message->payload_length = 0U;

	if (length < MPIPE_IPC_HEADER_LENGTH) {
		return -EMSGSIZE;
	}

	header.size = sys_get_le32(&buf[MPIPE_IPC_OFF_SIZE]);
	header.cmd = sys_get_le32(&buf[MPIPE_IPC_OFF_CMD]);
	header.generation = sys_get_le32(&buf[MPIPE_IPC_OFF_GENERATION]);

	/*
	 * Bound the declared size against both the protocol ceiling and the
	 * bytes actually present, before it is used as a length anywhere.
	 */
	if (header.size < MPIPE_IPC_HEADER_LENGTH ||
	    header.size > MPIPE_IPC_MAX_MESSAGE || header.size > length) {
		return -EMSGSIZE;
	}
	if (!type_is_known(MPIPE_IPC_CMD_TYPE(header.cmd))) {
		return -ENOTSUP;
	}
	if (header.generation == MPIPE_IPC_GENERATION_INVALID) {
		return -EPROTO;
	}

	payload_length = header.size - MPIPE_IPC_HEADER_LENGTH;

	message->header = header;
	message->payload_length = payload_length;
	message->payload = (payload_length != 0U) ? &buf[MPIPE_IPC_HEADER_LENGTH] : NULL;

	return 0;
}

int mpipe_ipc_validate_audio(const struct mpipe_ipc_message *message,
			     const struct mpipe_ipc_audio_format *expected)
{
	struct mpipe_ipc_audio_format actual;
	uint16_t type;
	uint8_t policy;

	if (message == NULL || expected == NULL) {
		return -EINVAL;
	}

	type = MPIPE_IPC_CMD_TYPE(message->header.cmd);
	if (type != MPIPE_IPC_TYPE_CONFIG && type != MPIPE_IPC_TYPE_CONFIG_ACK) {
		return -ENOTSUP;
	}
	if (message->payload == NULL || message->payload_length < FMT_LENGTH) {
		return -EMSGSIZE;
	}

	policy = message->payload[FMT_OFF_POLICY];

	actual.sample_rate = sys_get_le32(&message->payload[FMT_OFF_RATE]);
	actual.samples_per_channel = sys_get_le16(&message->payload[FMT_OFF_SAMPLES]);
	actual.channels = message->payload[FMT_OFF_CHANNELS];
	actual.sample_format = message->payload[FMT_OFF_FORMAT];
	actual.layout = message->payload[FMT_OFF_LAYOUT];
	actual.frame_duration_ms = message->payload[FMT_OFF_DURATION];
	actual.underrun_permitted = (policy & FMT_POLICY_UNDERRUN_PERMITTED) != 0U;
	actual.overrun_permitted = (policy & FMT_POLICY_OVERRUN_PERMITTED) != 0U;

	if (actual.sample_rate != expected->sample_rate ||
	    actual.samples_per_channel != expected->samples_per_channel ||
	    actual.channels != expected->channels ||
	    actual.sample_format != expected->sample_format ||
	    actual.layout != expected->layout ||
	    actual.frame_duration_ms != expected->frame_duration_ms ||
	    actual.underrun_permitted != expected->underrun_permitted ||
	    actual.overrun_permitted != expected->overrun_permitted) {
		return -EPROTO;
	}

	return 0;
}

int mpipe_ipc_audio_format_encode(uint8_t *buf, size_t buf_len,
				  const struct mpipe_ipc_audio_format *format)
{
	uint8_t policy = 0U;

	if (buf == NULL || format == NULL || buf_len < FMT_LENGTH) {
		return -EINVAL;
	}

	if (format->underrun_permitted) {
		policy |= FMT_POLICY_UNDERRUN_PERMITTED;
	}
	if (format->overrun_permitted) {
		policy |= FMT_POLICY_OVERRUN_PERMITTED;
	}

	sys_put_le32(format->sample_rate, &buf[FMT_OFF_RATE]);
	sys_put_le16(format->samples_per_channel, &buf[FMT_OFF_SAMPLES]);
	buf[FMT_OFF_CHANNELS] = format->channels;
	buf[FMT_OFF_FORMAT] = format->sample_format;
	buf[FMT_OFF_LAYOUT] = format->layout;
	buf[FMT_OFF_DURATION] = format->frame_duration_ms;
	buf[FMT_OFF_POLICY] = policy;
	buf[FMT_OFF_RESERVED] = 0U;

	return (int)FMT_LENGTH;
}
