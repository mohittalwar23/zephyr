/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Version 1 wire protocol for the direct M7 to HiFi4 link.
 *
 * The IPC Service backend moves opaque bytes between cores. This layer decides
 * whether those bytes are currently meaningful: it frames them, protects them
 * with CRC32C, and rejects anything structurally invalid before a payload is
 * exposed to audio processing.
 *
 * Wire fields are little-endian and are marshalled explicitly. A packed
 * structure is never cast over received bytes, so the format does not depend on
 * the host's endianness, structure padding, or the alignment of the receive
 * buffer.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PROTOCOL_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PROTOCOL_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Protocol version implemented here. */
#define MPIPE_IPC_VERSION_MAJOR 1U
#define MPIPE_IPC_VERSION_MINOR 0U

/** @brief Fixed version 1 header length, in bytes. */
#define MPIPE_IPC_HEADER_LENGTH 40U
/** @brief Largest complete message the transport exposes to the application. */
#define MPIPE_IPC_MAX_MESSAGE 1008U
/** @brief Largest payload that can follow a version 1 header. */
#define MPIPE_IPC_MAX_PAYLOAD (MPIPE_IPC_MAX_MESSAGE - MPIPE_IPC_HEADER_LENGTH)
/** @brief Length of the audio descriptor that precedes PCM bytes. */
#define MPIPE_IPC_AUDIO_DESC_LENGTH 16U
/** @brief Length of the prefix shared by every control payload. */
#define MPIPE_IPC_CTRL_PREFIX_LENGTH 8U

/** @brief Stream identifiers. */
#define MPIPE_IPC_STREAM_CONTROL 0U
#define MPIPE_IPC_STREAM_AUDIO   1U

/** @brief Message types assigned by version 1.0. */
enum mpipe_ipc_type {
	MPIPE_IPC_TYPE_HELLO = 0x01,
	MPIPE_IPC_TYPE_HELLO_ACK = 0x02,
	MPIPE_IPC_TYPE_CONFIG = 0x03,
	MPIPE_IPC_TYPE_CONFIG_ACK = 0x04,
	MPIPE_IPC_TYPE_START = 0x05,
	MPIPE_IPC_TYPE_START_ACK = 0x06,
	MPIPE_IPC_TYPE_STOP = 0x07,
	MPIPE_IPC_TYPE_STOP_ACK = 0x08,
	MPIPE_IPC_TYPE_CREDIT = 0x09,
	MPIPE_IPC_TYPE_STATUS = 0x0a,
	MPIPE_IPC_TYPE_ERROR = 0x0b,
	MPIPE_IPC_TYPE_HEARTBEAT = 0x0c,
	MPIPE_IPC_TYPE_HEARTBEAT_ACK = 0x0d,
	MPIPE_IPC_TYPE_AUDIO = 0x20,
};

/** @brief Header flag bits recognized by version 1.0. */
enum mpipe_ipc_flag {
	MPIPE_IPC_FLAG_ACK_REQUIRED = BIT(0),
	MPIPE_IPC_FLAG_RETRY = BIT(1),
	MPIPE_IPC_FLAG_DISCONTINUITY = BIT(2),
	MPIPE_IPC_FLAG_FATAL = BIT(3),
};

/** @brief Every flag this version understands. Bits 4..31 must be zero. */
#define MPIPE_IPC_FLAGS_KNOWN                                                          \
	((uint32_t)(MPIPE_IPC_FLAG_ACK_REQUIRED | MPIPE_IPC_FLAG_RETRY |               \
		    MPIPE_IPC_FLAG_DISCONTINUITY | MPIPE_IPC_FLAG_FATAL))

/** @brief Control transaction result codes. */
enum mpipe_ipc_result {
	MPIPE_IPC_RESULT_SUCCESS = 0,
	MPIPE_IPC_RESULT_UNSUPPORTED_VERSION = 1,
	MPIPE_IPC_RESULT_INVALID_STATE = 2,
	MPIPE_IPC_RESULT_INVALID_ARGUMENT = 3,
	MPIPE_IPC_RESULT_BUSY = 4,
	MPIPE_IPC_RESULT_INTERNAL = 5,
};

/** @brief Externally observable session states, shared by both peers. */
enum mpipe_ipc_state {
	MPIPE_IPC_STATE_BOOT = 0,
	MPIPE_IPC_STATE_BIND = 1,
	MPIPE_IPC_STATE_HELLO = 2,
	MPIPE_IPC_STATE_CONFIGURED = 3,
	MPIPE_IPC_STATE_PAUSED = 4,
	MPIPE_IPC_STATE_STREAMING = 5,
	MPIPE_IPC_STATE_DRAINING = 6,
	MPIPE_IPC_STATE_FAULT = 7,
};

/** @brief HELLO role codes. */
#define MPIPE_IPC_ROLE_HOST   1U
#define MPIPE_IPC_ROLE_REMOTE 2U

/** @brief Sample format and channel layout codes. */
#define MPIPE_IPC_FORMAT_S16LE       1U
#define MPIPE_IPC_LAYOUT_INTERLEAVED 1U

/** @brief STOP mode codes. */
#define MPIPE_IPC_STOP_MODE_DRAIN 1U

/** @brief Complete version 1 audio frame length: header, descriptor, PCM. */
#define MPIPE_IPC_V1_AUDIO_FRAME_LENGTH                                                \
	(MPIPE_IPC_HEADER_LENGTH + MPIPE_IPC_AUDIO_DESC_LENGTH + 640U)

/** @brief Decoded form of the 40-byte wire header, in host byte order. */
struct mpipe_ipc_header {
	uint8_t major;
	uint8_t minor;
	uint8_t type;
	uint8_t header_length;
	uint32_t flags;
	uint32_t session_id;
	uint32_t stream_id;
	uint32_t sequence;
	uint32_t payload_length;
	uint32_t timestamp_us;
	uint32_t format_generation;
	uint32_t crc32c;
};

/**
 * @brief A decoded message: its header plus a span into caller-owned storage.
 *
 * After a successful mpipe_ipc_decode() the payload points into the caller's
 * receive buffer and stays valid only as long as that buffer does.
 */
struct mpipe_ipc_message {
	struct mpipe_ipc_header header;
	const uint8_t *payload;
	size_t payload_length;
};

/** @brief The 16-byte audio descriptor that precedes PCM bytes. */
struct mpipe_ipc_audio_format {
	uint32_t sample_rate;
	uint16_t samples_per_channel;
	uint8_t channels;
	uint8_t sample_format;
	uint8_t layout;
	uint8_t frame_duration_ms;
	uint32_t pcm_bytes;
};

/** @brief The single audio format version 1 permits. */
#define MPIPE_IPC_V1_AUDIO_FORMAT                                                      \
	{                                                                              \
		.sample_rate = 16000U,                                                 \
		.samples_per_channel = 160U,                                           \
		.channels = 2U,                                                        \
		.sample_format = MPIPE_IPC_FORMAT_S16LE,                               \
		.layout = MPIPE_IPC_LAYOUT_INTERLEAVED,                                \
		.frame_duration_ms = 10U,                                              \
		.pcm_bytes = 640U,                                                     \
	}

BUILD_ASSERT(MPIPE_IPC_HEADER_LENGTH == 40U, "v1 header is 40 bytes");
BUILD_ASSERT(MPIPE_IPC_AUDIO_DESC_LENGTH == 16U, "v1 audio descriptor is 16 bytes");
BUILD_ASSERT(MPIPE_IPC_CTRL_PREFIX_LENGTH == 8U, "v1 control prefix is 8 bytes");
BUILD_ASSERT(MPIPE_IPC_MAX_PAYLOAD == 968U, "v1 payload ceiling");
BUILD_ASSERT(MPIPE_IPC_V1_AUDIO_FRAME_LENGTH == 696U, "v1 audio frame is 696 bytes");
BUILD_ASSERT(MPIPE_IPC_V1_AUDIO_FRAME_LENGTH <= MPIPE_IPC_MAX_MESSAGE,
	     "v1 audio frame must fit the application-visible maximum");

/**
 * @brief Serialize a message and fill in its CRC32C.
 *
 * The header's @c payload_length and @c crc32c are ignored on input and are
 * written from @c message->payload_length and the computed checksum.
 *
 * @param dst      Destination buffer.
 * @param capacity Bytes available in @p dst.
 * @param message  Message to serialize.
 * @param written  Set to the complete frame length on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL   A pointer is NULL, or a payload is declared without bytes.
 * @retval -EMSGSIZE The payload exceeds MPIPE_IPC_MAX_PAYLOAD.
 * @retval -ENOSPC   @p capacity cannot hold the complete frame.
 */
int mpipe_ipc_encode(void *dst, size_t capacity, const struct mpipe_ipc_message *message,
		     size_t *written);

/**
 * @brief Validate and decode a received frame.
 *
 * Applies every version 1 receive rule before exposing any payload, in this
 * order: buffer length, magic, major version, header length, reserved flag
 * bits, nonzero session, known type, known stream, declared payload bounds,
 * available bytes, and finally CRC32C. On any rejection the message's payload
 * span is cleared, so a caller cannot follow a pointer into rejected bytes.
 *
 * @param message Filled in on success.
 * @param src     Received bytes; may be unaligned.
 * @param length  Bytes available in @p src.
 *
 * @retval 0 on success.
 * @retval -EINVAL           A pointer is NULL.
 * @retval -EMSGSIZE         Truncated, wrong header length, or a declared
 *                           payload that is out of range or not present.
 * @retval -EBADMSG          Wrong magic, or the CRC32C does not match.
 * @retval -EPROTONOSUPPORT  Major version mismatch.
 * @retval -ENOTSUP          Unknown message type or unknown mandatory flag.
 * @retval -EPROTO           Zero session identifier or unknown stream.
 */
int mpipe_ipc_decode(struct mpipe_ipc_message *message, const void *src, size_t length);

/**
 * @brief Check that a decoded audio message carries the expected format.
 *
 * @retval 0 when the descriptor matches @p expected and the PCM length agrees.
 * @retval -EINVAL   A pointer is NULL.
 * @retval -ENOTSUP  The message is not an audio message.
 * @retval -EMSGSIZE The payload is too short, or @c pcm_bytes disagrees with it.
 * @retval -EILSEQ   The descriptor's reserved field is nonzero.
 * @retval -EPROTO   The descriptor differs from @p expected.
 */
int mpipe_ipc_validate_audio(const struct mpipe_ipc_message *message,
			     const struct mpipe_ipc_audio_format *expected);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PROTOCOL_H_ */
