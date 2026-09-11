/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/crc.h>
#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>

#include "mpipe_ipc_vectors.h"

ZTEST_SUITE(mpipe_ipc_protocol, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Golden vectors                                                      */
/* ------------------------------------------------------------------ */

/*
 * Guards the toolchain against the spec's exact CRC32C parameters. The
 * generator asserts the same value independently in Python, so a disagreement
 * here means the C side, not the vector file, has drifted.
 */
ZTEST(mpipe_ipc_protocol, test_crc32c_check_value)
{
	const char *input = MPIPE_IPC_VECTOR_CRC_CHECK_INPUT;

	zassert_equal(crc32_c(0, (const uint8_t *)input, strlen(input), true, true),
		      MPIPE_IPC_VECTOR_CRC_CHECK_EXPECTED);
}

static void message_from_vector(struct mpipe_ipc_message *message,
				const struct mpipe_ipc_vector *vector)
{
	memset(message, 0, sizeof(*message));
	message->header.major = vector->major;
	message->header.minor = vector->minor;
	message->header.type = vector->type;
	message->header.header_length = vector->header_length;
	message->header.flags = vector->flags;
	message->header.session_id = vector->session_id;
	message->header.stream_id = vector->stream_id;
	message->header.sequence = vector->sequence;
	message->header.timestamp_us = vector->timestamp_us;
	message->header.format_generation = vector->format_generation;
	message->payload = vector->frame + MPIPE_IPC_HEADER_LENGTH;
	message->payload_length = vector->payload_length;
}

ZTEST(mpipe_ipc_protocol, test_every_golden_frame_encodes_to_exact_bytes)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		const struct mpipe_ipc_vector *vector = &mpipe_ipc_vectors[i];
		static uint8_t encoded[MPIPE_IPC_MAX_MESSAGE];
		struct mpipe_ipc_message message;
		size_t written = 0;

		message_from_vector(&message, vector);

		zassert_ok(mpipe_ipc_encode(encoded, sizeof(encoded), &message, &written),
			   "vector %s failed to encode", vector->name);
		zassert_equal(written, vector->frame_length, "vector %s length",
			      vector->name);
		zassert_mem_equal(encoded, vector->frame, vector->frame_length,
				  "vector %s bytes differ from the golden frame",
				  vector->name);
	}
}

ZTEST(mpipe_ipc_protocol, test_every_golden_frame_decodes_to_expected_fields)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		const struct mpipe_ipc_vector *vector = &mpipe_ipc_vectors[i];
		struct mpipe_ipc_message message;

		zassert_ok(mpipe_ipc_decode(&message, vector->frame,
					    vector->frame_length),
			   "vector %s failed to decode", vector->name);

		zassert_equal(message.header.type, vector->type, "%s type", vector->name);
		zassert_equal(message.header.flags, vector->flags, "%s flags",
			      vector->name);
		zassert_equal(message.header.session_id, vector->session_id, "%s session",
			      vector->name);
		zassert_equal(message.header.stream_id, vector->stream_id, "%s stream",
			      vector->name);
		zassert_equal(message.header.sequence, vector->sequence, "%s sequence",
			      vector->name);
		zassert_equal(message.header.timestamp_us, vector->timestamp_us,
			      "%s timestamp", vector->name);
		zassert_equal(message.header.format_generation, vector->format_generation,
			      "%s format generation", vector->name);
		zassert_equal(message.header.crc32c, vector->crc32c, "%s crc",
			      vector->name);
		zassert_equal(message.payload_length, vector->payload_length,
			      "%s payload length", vector->name);

		if (vector->payload_length != 0U) {
			zassert_mem_equal(message.payload,
					  vector->frame + MPIPE_IPC_HEADER_LENGTH,
					  vector->payload_length, "%s payload",
					  vector->name);
		}
	}
}

static const struct mpipe_ipc_vector *vector_named(const char *name)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		if (strcmp(mpipe_ipc_vectors[i].name, name) == 0) {
			return &mpipe_ipc_vectors[i];
		}
	}

	return NULL;
}

ZTEST(mpipe_ipc_protocol, test_v1_audio_frame_is_696_bytes_and_validates)
{
	const struct mpipe_ipc_vector *vector = vector_named("audio_frame");
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;

	zassert_not_null(vector, "the audio_frame vector must exist");
	zassert_equal(vector->frame_length, 696U);
	zassert_true(vector->frame_length <= MPIPE_IPC_MAX_MESSAGE);

	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));
	zassert_equal(message.header.stream_id, MPIPE_IPC_STREAM_AUDIO);
	zassert_ok(mpipe_ipc_validate_audio(&message, &expected));
}

ZTEST(mpipe_ipc_protocol, test_audio_validation_rejects_a_different_format)
{
	const struct mpipe_ipc_vector *vector = vector_named("audio_frame");
	struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));

	expected.sample_rate = 48000U;
	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -EPROTO);
}

ZTEST(mpipe_ipc_protocol, test_audio_validation_rejects_a_control_message)
{
	const struct mpipe_ipc_vector *vector = vector_named("heartbeat_request");
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));
	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -ENOTSUP);
}

/*
 * The wire format must not depend on the alignment of the receive buffer, so
 * decode a golden frame from an odd offset. A packed-struct cast would fault
 * or silently misread here on a strict-alignment target.
 */
ZTEST(mpipe_ipc_protocol, test_decode_accepts_an_unaligned_buffer)
{
	const struct mpipe_ipc_vector *vector = vector_named("status_report");
	static uint8_t staging[MPIPE_IPC_MAX_MESSAGE + 3];
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	memcpy(&staging[1], vector->frame, vector->frame_length);

	zassert_ok(mpipe_ipc_decode(&message, &staging[1], vector->frame_length));
	zassert_equal(message.header.crc32c, vector->crc32c);
}

/* ------------------------------------------------------------------ */
/* Rejection table                                                     */
/* ------------------------------------------------------------------ */

enum mutate_kind {
	MUTATE_NONE,
	MUTATE_BYTE,
	MUTATE_LE32,
	MUTATE_TRUNCATE,
};

struct reject_case {
	const char *name;
	enum mutate_kind kind;
	size_t offset;
	uint32_t value;
	size_t length_override;
	int expected;
};

static const struct reject_case reject_cases[] = {
	{ .name = "bad magic", .kind = MUTATE_BYTE, .offset = 0, .value = 'X',
	  .expected = -EBADMSG },
	{ .name = "unsupported major version", .kind = MUTATE_BYTE, .offset = 4,
	  .value = 2, .expected = -EPROTONOSUPPORT },
	{ .name = "wrong header length", .kind = MUTATE_BYTE, .offset = 7, .value = 44,
	  .expected = -EMSGSIZE },
	{ .name = "unknown mandatory flag", .kind = MUTATE_LE32, .offset = 8,
	  .value = 0x00000100U, .expected = -ENOTSUP },
	{ .name = "zero session identifier", .kind = MUTATE_LE32, .offset = 12,
	  .value = 0U, .expected = -EPROTO },
	{ .name = "unknown message type", .kind = MUTATE_BYTE, .offset = 6, .value = 0x7f,
	  .expected = -ENOTSUP },
	{ .name = "unknown stream identifier", .kind = MUTATE_LE32, .offset = 16,
	  .value = 9U, .expected = -EPROTO },
	{ .name = "payload length beyond the protocol maximum", .kind = MUTATE_LE32,
	  .offset = 24, .value = MPIPE_IPC_MAX_MESSAGE, .expected = -EMSGSIZE },
	{ .name = "payload length overflows the addition", .kind = MUTATE_LE32,
	  .offset = 24, .value = 0xffffffffU, .expected = -EMSGSIZE },
	{ .name = "declared payload longer than the buffer", .kind = MUTATE_LE32,
	  .offset = 24, .value = 64U, .expected = -EMSGSIZE },
	{ .name = "corrupt crc", .kind = MUTATE_LE32, .offset = 36, .value = 0xdeadbeefU,
	  .expected = -EBADMSG },
	{ .name = "corrupt payload", .kind = MUTATE_BYTE, .offset = 40, .value = 0xff,
	  .expected = -EBADMSG },
	{ .name = "truncated below a full header", .kind = MUTATE_TRUNCATE,
	  .length_override = MPIPE_IPC_HEADER_LENGTH - 1, .expected = -EMSGSIZE },
	{ .name = "truncated payload", .kind = MUTATE_TRUNCATE,
	  .length_override = MPIPE_IPC_HEADER_LENGTH + 1, .expected = -EMSGSIZE },
};

ZTEST(mpipe_ipc_protocol, test_malformed_frames_are_rejected)
{
	const struct mpipe_ipc_vector *vector = vector_named("status_report");

	zassert_not_null(vector);

	for (size_t i = 0; i < ARRAY_SIZE(reject_cases); i++) {
		const struct reject_case *test = &reject_cases[i];
		static uint8_t frame[MPIPE_IPC_MAX_MESSAGE];
		struct mpipe_ipc_message message;
		size_t length = vector->frame_length;

		memcpy(frame, vector->frame, vector->frame_length);

		switch (test->kind) {
		case MUTATE_BYTE:
			frame[test->offset] = (uint8_t)test->value;
			break;
		case MUTATE_LE32:
			sys_put_le32(test->value, &frame[test->offset]);
			break;
		case MUTATE_TRUNCATE:
			length = test->length_override;
			break;
		case MUTATE_NONE:
		default:
			break;
		}

		zassert_equal(mpipe_ipc_decode(&message, frame, length), test->expected,
			      "case %zu (%s) returned the wrong rejection", i,
			      test->name);
	}
}

/* A rejected frame must never leave a payload pointer a caller could follow. */
ZTEST(mpipe_ipc_protocol, test_rejected_frame_leaves_no_payload_pointer)
{
	const struct mpipe_ipc_vector *vector = vector_named("audio_frame");
	static uint8_t frame[MPIPE_IPC_MAX_MESSAGE];
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	memcpy(frame, vector->frame, vector->frame_length);
	frame[MPIPE_IPC_HEADER_LENGTH + 8] ^= 0xffU;

	message.payload = (const uint8_t *)0x1;
	message.payload_length = 12345U;

	zassert_equal(mpipe_ipc_decode(&message, frame, vector->frame_length),
		      -EBADMSG);
	zassert_is_null(message.payload, "payload must not be exposed on rejection");
	zassert_equal(message.payload_length, 0U);
}

ZTEST(mpipe_ipc_protocol, test_encode_argument_and_capacity_rules)
{
	const struct mpipe_ipc_vector *vector = vector_named("heartbeat_request");
	static uint8_t encoded[MPIPE_IPC_MAX_MESSAGE];
	struct mpipe_ipc_message message;
	size_t written = 0;

	zassert_not_null(vector);
	message_from_vector(&message, vector);

	zassert_equal(mpipe_ipc_encode(NULL, sizeof(encoded), &message, &written),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(encoded, sizeof(encoded), NULL, &written),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(encoded, sizeof(encoded), &message, NULL),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(encoded, vector->frame_length - 1, &message,
				       &written),
		      -ENOSPC);

	message.payload_length = MPIPE_IPC_MAX_MESSAGE;
	zassert_equal(mpipe_ipc_encode(encoded, sizeof(encoded), &message, &written),
		      -EMSGSIZE);
}

ZTEST(mpipe_ipc_protocol, test_decode_argument_rules)
{
	const struct mpipe_ipc_vector *vector = vector_named("heartbeat_request");
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_equal(mpipe_ipc_decode(NULL, vector->frame, vector->frame_length),
		      -EINVAL);
	zassert_equal(mpipe_ipc_decode(&message, NULL, vector->frame_length), -EINVAL);
}

ZTEST(mpipe_ipc_protocol, test_encode_then_decode_round_trips_every_vector)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		const struct mpipe_ipc_vector *vector = &mpipe_ipc_vectors[i];
		static uint8_t encoded[MPIPE_IPC_MAX_MESSAGE];
		struct mpipe_ipc_message out;
		struct mpipe_ipc_message in;
		size_t written = 0;

		message_from_vector(&in, vector);
		zassert_ok(mpipe_ipc_encode(encoded, sizeof(encoded), &in, &written));
		zassert_ok(mpipe_ipc_decode(&out, encoded, written));

		zassert_equal(out.header.type, in.header.type, "%s", vector->name);
		zassert_equal(out.header.sequence, in.header.sequence, "%s",
			      vector->name);
		zassert_equal(out.payload_length, in.payload_length, "%s", vector->name);
	}
}
