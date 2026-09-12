/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Version 1 control protocol for the direct M7 to HiFi4 link.
 *
 * This layer frames **control** messages only. Bulk audio does not travel
 * here: it moves through a dedicated shared-DDR ring with its own doorbell,
 * following the same split that NXP AN12762 and SOF both use, so a real-time
 * bulk path never shares a strictly one-outstanding control channel.
 *
 * The header is deliberately small. SOF ships an 8-byte IPC header in
 * production on this same HiFi4 over this same doorbell-plus-shared-window
 * transport (@c sof_ipc_cmd_hdr, @c src/include/ipc/header.h), and NXP's SRTM
 * ships 10 bytes. This header is 12: SOF's two fields plus the one thing
 * neither vendor needs and this design does.
 *
 * That extra field is @c generation. In both SOF and SRTM the lifecycle
 * authority and the data-plane peer are the same entity, so neither can meet
 * a peer that restarted independently. Here Linux is the lifecycle authority
 * but the M7 and the HiFi4 are the data-plane peers, so either can be
 * restarted under the other. Each core publishes a boot generation and stamps
 * it on every message; a peer that restarted is then rejected per message,
 * with no state machine required to notice.
 *
 * Deliberately absent, each following established practice rather than
 * oversight:
 *
 * - **No CRC.** SOF carries none on a shared-DDR path. A checksum cannot
 *   detect the failure that actually occurs here, a stale cache line, because
 *   it is computed over the same stale bytes and validates. Cache maintenance
 *   is the correctness requirement.
 * - **No magic.** SOF reserves its magic for data tunnelled from userspace
 *   and omits it core-to-core. This is the trusted path.
 * - **No per-message version.** The ABI is negotiated once in HELLO, and only
 *   the major version must match.
 * - **No per-message sequence.** A doorbell-ordered channel does not reorder
 *   or duplicate. Sequence belongs on the lossy audio ring, for gap
 *   statistics, not here.
 *
 * Fields are marshalled explicitly at named offsets, little-endian. A packed
 * structure is never cast over received bytes, so the format does not depend
 * on host endianness, structure padding, or receive-buffer alignment.
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

/** @brief Protocol version. Only the major version must match a peer. */
#define MPIPE_IPC_VERSION_MAJOR 1U
#define MPIPE_IPC_VERSION_MINOR 0U

/** @brief Fixed control header length, in bytes. */
#define MPIPE_IPC_HEADER_LENGTH 12U
/** @brief Largest control message, including the header. */
#define MPIPE_IPC_MAX_MESSAGE 256U
/** @brief Largest control payload that can follow a header. */
#define MPIPE_IPC_MAX_PAYLOAD (MPIPE_IPC_MAX_MESSAGE - MPIPE_IPC_HEADER_LENGTH)

/** @brief Header field offsets, in bytes from the start of the message. */
#define MPIPE_IPC_OFF_SIZE       0U
#define MPIPE_IPC_OFF_CMD        4U
#define MPIPE_IPC_OFF_GENERATION 8U

/**
 * @brief @c cmd encoding: message type in bits 31:16, identifier in 15:0.
 *
 * The identifier carries the control transaction id, so a reply can be
 * matched to its request without a separate field.
 */
#define MPIPE_IPC_CMD_TYPE_SHIFT 16U
#define MPIPE_IPC_CMD_ID_MASK    0xffffU

#define MPIPE_IPC_CMD(type, id)                                                        \
	((((uint32_t)(type)) << MPIPE_IPC_CMD_TYPE_SHIFT) |                            \
	 ((uint32_t)(id) & MPIPE_IPC_CMD_ID_MASK))
#define MPIPE_IPC_CMD_TYPE(cmd) ((uint16_t)((cmd) >> MPIPE_IPC_CMD_TYPE_SHIFT))
#define MPIPE_IPC_CMD_ID(cmd)   ((uint16_t)((cmd) & MPIPE_IPC_CMD_ID_MASK))

/**
 * @brief Control message types.
 *
 * There is no audio type: audio does not travel on this path. There is no
 * credit type either; a slow consumer is governed by the per-stream
 * @c underrun_permitted / @c overrun_permitted policy, not by a window.
 */
enum mpipe_ipc_type {
	MPIPE_IPC_TYPE_HELLO = 0x01,
	MPIPE_IPC_TYPE_HELLO_ACK = 0x02,
	MPIPE_IPC_TYPE_CONFIG = 0x03,
	MPIPE_IPC_TYPE_CONFIG_ACK = 0x04,
	MPIPE_IPC_TYPE_START = 0x05,
	MPIPE_IPC_TYPE_START_ACK = 0x06,
	MPIPE_IPC_TYPE_STOP = 0x07,
	MPIPE_IPC_TYPE_STOP_ACK = 0x08,
	MPIPE_IPC_TYPE_STATUS = 0x09,
	MPIPE_IPC_TYPE_ERROR = 0x0a,
	MPIPE_IPC_TYPE_HEARTBEAT = 0x0b,
	MPIPE_IPC_TYPE_HEARTBEAT_ACK = 0x0c,
};

/** @brief Control transaction result codes. */
enum mpipe_ipc_result {
	MPIPE_IPC_RESULT_SUCCESS = 0,
	MPIPE_IPC_RESULT_UNSUPPORTED_VERSION = 1,
	MPIPE_IPC_RESULT_INVALID_STATE = 2,
	MPIPE_IPC_RESULT_INVALID_ARGUMENT = 3,
	MPIPE_IPC_RESULT_BUSY = 4,
	MPIPE_IPC_RESULT_INTERNAL = 5,
};

/** @brief HELLO role codes. */
#define MPIPE_IPC_ROLE_HOST   1U
#define MPIPE_IPC_ROLE_REMOTE 2U

/** @brief A generation is never zero; zero means "not yet published". */
#define MPIPE_IPC_GENERATION_INVALID 0U

/** @brief Decoded control header, in host byte order. */
struct mpipe_ipc_header {
	uint32_t size;
	uint32_t cmd;
	uint32_t generation;
};

/**
 * @brief A decoded message: its header plus a span into caller-owned storage.
 *
 * After a successful decode the payload points into the caller's receive
 * buffer and stays valid only as long as that buffer does.
 */
struct mpipe_ipc_message {
	struct mpipe_ipc_header header;
	const uint8_t *payload;
	size_t payload_length;
};

/**
 * @brief Audio format, exchanged in CONFIG and negotiated once.
 *
 * This describes the stream carried by the audio ring; it never appears in an
 * audio message, because there are none.
 */
struct mpipe_ipc_audio_format {
	uint32_t sample_rate;
	uint16_t samples_per_channel;
	uint8_t channels;
	uint8_t sample_format;
	uint8_t layout;
	uint8_t frame_duration_ms;
	/**
	 * Slow-consumer policy, following SOF's per-stream xrun permissions.
	 * When permitted, the ring drops and continues; otherwise the session
	 * stops and reports.
	 */
	bool underrun_permitted;
	bool overrun_permitted;
};

/** @brief Sample format and channel layout codes. */
#define MPIPE_IPC_FORMAT_S16LE       1U
#define MPIPE_IPC_LAYOUT_INTERLEAVED 1U

/** @brief The single audio format version 1 permits. */
#define MPIPE_IPC_V1_AUDIO_FORMAT                                                      \
	{                                                                              \
		.sample_rate = 16000U,                                                 \
		.samples_per_channel = 160U,                                           \
		.channels = 2U,                                                        \
		.sample_format = MPIPE_IPC_FORMAT_S16LE,                               \
		.layout = MPIPE_IPC_LAYOUT_INTERLEAVED,                                \
		.frame_duration_ms = 10U,                                              \
		.underrun_permitted = false,                                           \
		.overrun_permitted = true,                                             \
	}

/** @brief Serialized length of an audio format in a CONFIG payload. */
#define MPIPE_IPC_AUDIO_FORMAT_LENGTH 12U

/** @brief PCM bytes in one version 1 audio period, carried on the ring. */
#define MPIPE_IPC_V1_PCM_BYTES 640U

BUILD_ASSERT(MPIPE_IPC_HEADER_LENGTH == 12U, "v1 control header is 12 bytes");
BUILD_ASSERT(MPIPE_IPC_OFF_GENERATION + sizeof(uint32_t) == MPIPE_IPC_HEADER_LENGTH,
	     "header offsets must tile the header exactly");
BUILD_ASSERT(MPIPE_IPC_MAX_PAYLOAD == 244U, "v1 control payload ceiling");
BUILD_ASSERT(MPIPE_IPC_AUDIO_FORMAT_LENGTH == 12U, "v1 audio format is 12 bytes");

/**
 * @brief Serialize a control message.
 *
 * The header's @c size is ignored on input and written from @p payload_length.
 *
 * @retval 0 on success, with @p written set to the complete message length.
 * @retval -EINVAL   A pointer is NULL, or a payload is declared without bytes.
 * @retval -EMSGSIZE The payload exceeds MPIPE_IPC_MAX_PAYLOAD.
 * @retval -ENOSPC   @p capacity cannot hold the complete message.
 */
int mpipe_ipc_encode(void *dst, size_t capacity, const struct mpipe_ipc_message *message,
		     size_t *written);

/**
 * @brief Validate and decode a received control message.
 *
 * Applies every receive rule before exposing a payload: available length,
 * declared size bounds, a known message type, and a nonzero generation. On
 * rejection the payload span is cleared, so a caller cannot follow a pointer
 * into rejected bytes.
 *
 * The generation is returned but not judged here; the session layer compares
 * it against the value latched from the peer at session open.
 *
 * @retval 0 on success.
 * @retval -EINVAL   A pointer is NULL.
 * @retval -EMSGSIZE Truncated, or a declared size out of range or not present.
 * @retval -ENOTSUP  Unknown message type.
 * @retval -EPROTO   Generation is zero.
 */
int mpipe_ipc_decode(struct mpipe_ipc_message *message, const void *src, size_t length);

/**
 * @brief Check a decoded CONFIG payload against the expected audio format.
 *
 * @retval 0 when the format matches @p expected.
 * @retval -EINVAL   A pointer is NULL.
 * @retval -ENOTSUP  The message is not CONFIG or CONFIG_ACK.
 * @retval -EMSGSIZE The payload is too short to hold a format.
 * @retval -EPROTO   The format differs from @p expected.
 */
int mpipe_ipc_validate_audio(const struct mpipe_ipc_message *message,
			     const struct mpipe_ipc_audio_format *expected);

/**
 * @brief Serialize an audio format into a CONFIG payload.
 *
 * @retval MPIPE_IPC_AUDIO_FORMAT_LENGTH on success.
 * @retval -EINVAL A pointer is NULL or @p buf_len is too small.
 */
int mpipe_ipc_audio_format_encode(uint8_t *buf, size_t buf_len,
				  const struct mpipe_ipc_audio_format *format);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PROTOCOL_H_ */
