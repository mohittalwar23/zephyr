/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/sys/crc.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>

/* Header field offsets, in bytes from the start of the frame. */
#define OFF_MAGIC      0U
#define OFF_MAJOR      4U
#define OFF_MINOR      5U
#define OFF_TYPE       6U
#define OFF_HDR_LENGTH 7U
#define OFF_FLAGS      8U
#define OFF_SESSION    12U
#define OFF_STREAM     16U
#define OFF_SEQUENCE   20U
#define OFF_PAYLOAD    24U
#define OFF_TIMESTAMP  28U
#define OFF_FORMAT_GEN 32U
#define OFF_CRC        36U

/* Audio descriptor field offsets, from the start of the payload. */
#define DESC_OFF_RATE     0U
#define DESC_OFF_SAMPLES  4U
#define DESC_OFF_CHANNELS 6U
#define DESC_OFF_FORMAT   7U
#define DESC_OFF_LAYOUT   8U
#define DESC_OFF_DURATION 9U
#define DESC_OFF_RESERVED 10U
#define DESC_OFF_PCM      12U

static const uint8_t mpipe_ipc_magic[4] = { 'M', 'I', 'P', 'C' };

static bool type_is_known(uint8_t type)
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
	case MPIPE_IPC_TYPE_CREDIT:
	case MPIPE_IPC_TYPE_STATUS:
	case MPIPE_IPC_TYPE_ERROR:
	case MPIPE_IPC_TYPE_HEARTBEAT:
	case MPIPE_IPC_TYPE_HEARTBEAT_ACK:
	case MPIPE_IPC_TYPE_AUDIO:
		return true;
	default:
		return false;
	}
}

/*
 * The CRC covers the 40-byte header with its own CRC field treated as zero,
 * then the payload. The header is copied into a local so the caller's buffer is
 * never modified and so an unaligned source is handled by memcpy rather than by
 * a structure overlay.
 */
static uint32_t frame_crc32c(const uint8_t *frame, size_t payload_length)
{
	uint8_t header[MPIPE_IPC_HEADER_LENGTH];
	uint32_t crc;

	memcpy(header, frame, sizeof(header));
	memset(&header[OFF_CRC], 0, sizeof(uint32_t));

	crc = crc32_c(0U, header, sizeof(header), true, payload_length == 0U);
	if (payload_length != 0U) {
		crc = crc32_c(crc, &frame[MPIPE_IPC_HEADER_LENGTH], payload_length, false,
			      true);
	}

	return crc;
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

	memcpy(&buf[OFF_MAGIC], mpipe_ipc_magic, sizeof(mpipe_ipc_magic));
	buf[OFF_MAJOR] = message->header.major;
	buf[OFF_MINOR] = message->header.minor;
	buf[OFF_TYPE] = message->header.type;
	buf[OFF_HDR_LENGTH] = message->header.header_length;
	sys_put_le32(message->header.flags, &buf[OFF_FLAGS]);
	sys_put_le32(message->header.session_id, &buf[OFF_SESSION]);
	sys_put_le32(message->header.stream_id, &buf[OFF_STREAM]);
	sys_put_le32(message->header.sequence, &buf[OFF_SEQUENCE]);
	sys_put_le32((uint32_t)message->payload_length, &buf[OFF_PAYLOAD]);
	sys_put_le32(message->header.timestamp_us, &buf[OFF_TIMESTAMP]);
	sys_put_le32(message->header.format_generation, &buf[OFF_FORMAT_GEN]);
	sys_put_le32(0U, &buf[OFF_CRC]);

	if (message->payload_length != 0U) {
		memcpy(&buf[MPIPE_IPC_HEADER_LENGTH], message->payload,
		       message->payload_length);
	}

	sys_put_le32(frame_crc32c(buf, message->payload_length), &buf[OFF_CRC]);

	*written = total;

	return 0;
}

int mpipe_ipc_decode(struct mpipe_ipc_message *message, const void *src, size_t length)
{
	const uint8_t *buf = src;
	struct mpipe_ipc_header header;
	size_t payload_length;
	size_t total;

	if (message == NULL || src == NULL) {
		return -EINVAL;
	}

	/* Nothing may be exposed until every rule below has passed. */
	message->payload = NULL;
	message->payload_length = 0U;

	if (length < MPIPE_IPC_HEADER_LENGTH) {
		return -EMSGSIZE;
	}
	if (memcmp(&buf[OFF_MAGIC], mpipe_ipc_magic, sizeof(mpipe_ipc_magic)) != 0) {
		return -EBADMSG;
	}

	header.major = buf[OFF_MAJOR];
	header.minor = buf[OFF_MINOR];
	header.type = buf[OFF_TYPE];
	header.header_length = buf[OFF_HDR_LENGTH];
	header.flags = sys_get_le32(&buf[OFF_FLAGS]);
	header.session_id = sys_get_le32(&buf[OFF_SESSION]);
	header.stream_id = sys_get_le32(&buf[OFF_STREAM]);
	header.sequence = sys_get_le32(&buf[OFF_SEQUENCE]);
	header.payload_length = sys_get_le32(&buf[OFF_PAYLOAD]);
	header.timestamp_us = sys_get_le32(&buf[OFF_TIMESTAMP]);
	header.format_generation = sys_get_le32(&buf[OFF_FORMAT_GEN]);
	header.crc32c = sys_get_le32(&buf[OFF_CRC]);

	if (header.major != MPIPE_IPC_VERSION_MAJOR) {
		return -EPROTONOSUPPORT;
	}
	if (header.header_length != MPIPE_IPC_HEADER_LENGTH) {
		return -EMSGSIZE;
	}
	if ((header.flags & ~MPIPE_IPC_FLAGS_KNOWN) != 0U) {
		return -ENOTSUP;
	}
	if (header.session_id == 0U) {
		return -EPROTO;
	}
	if (!type_is_known(header.type)) {
		return -ENOTSUP;
	}
	if (header.stream_id != MPIPE_IPC_STREAM_CONTROL &&
	    header.stream_id != MPIPE_IPC_STREAM_AUDIO) {
		return -EPROTO;
	}

	/*
	 * Bound the declared length before adding it to the header length, so
	 * a hostile or corrupt value cannot overflow the sum.
	 */
	if (header.payload_length > MPIPE_IPC_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	payload_length = (size_t)header.payload_length;
	total = MPIPE_IPC_HEADER_LENGTH + payload_length;
	if (length < total) {
		return -EMSGSIZE;
	}

	if (frame_crc32c(buf, payload_length) != header.crc32c) {
		return -EBADMSG;
	}

	message->header = header;
	message->payload_length = payload_length;
	message->payload = (payload_length != 0U) ? &buf[MPIPE_IPC_HEADER_LENGTH] : NULL;

	return 0;
}

int mpipe_ipc_validate_audio(const struct mpipe_ipc_message *message,
			     const struct mpipe_ipc_audio_format *expected)
{
	struct mpipe_ipc_audio_format actual;
	size_t pcm_available;

	if (message == NULL || expected == NULL) {
		return -EINVAL;
	}
	if (message->header.type != MPIPE_IPC_TYPE_AUDIO) {
		return -ENOTSUP;
	}
	if (message->payload == NULL ||
	    message->payload_length < MPIPE_IPC_AUDIO_DESC_LENGTH) {
		return -EMSGSIZE;
	}

	if (sys_get_le16(&message->payload[DESC_OFF_RESERVED]) != 0U) {
		return -EILSEQ;
	}

	actual.sample_rate = sys_get_le32(&message->payload[DESC_OFF_RATE]);
	actual.samples_per_channel = sys_get_le16(&message->payload[DESC_OFF_SAMPLES]);
	actual.channels = message->payload[DESC_OFF_CHANNELS];
	actual.sample_format = message->payload[DESC_OFF_FORMAT];
	actual.layout = message->payload[DESC_OFF_LAYOUT];
	actual.frame_duration_ms = message->payload[DESC_OFF_DURATION];
	actual.pcm_bytes = sys_get_le32(&message->payload[DESC_OFF_PCM]);

	pcm_available = message->payload_length - MPIPE_IPC_AUDIO_DESC_LENGTH;
	if (actual.pcm_bytes != pcm_available) {
		return -EMSGSIZE;
	}

	if (actual.sample_rate != expected->sample_rate ||
	    actual.samples_per_channel != expected->samples_per_channel ||
	    actual.channels != expected->channels ||
	    actual.sample_format != expected->sample_format ||
	    actual.layout != expected->layout ||
	    actual.frame_duration_ms != expected->frame_duration_ms ||
	    actual.pcm_bytes != expected->pcm_bytes) {
		return -EPROTO;
	}

	return 0;
}
