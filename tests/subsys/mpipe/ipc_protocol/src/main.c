/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>

#include "mpipe_ipc_vectors.h"

ZTEST_SUITE(mpipe_ipc_protocol, NULL, NULL, NULL, NULL, NULL);

/* Kilobyte-scale scratch must not live on the 1024-byte ztest stack. */
static uint8_t scratch[MPIPE_IPC_MAX_MESSAGE];

static const struct mpipe_ipc_vector *vector_named(const char *name)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		if (strcmp(mpipe_ipc_vectors[i].name, name) == 0) {
			return &mpipe_ipc_vectors[i];
		}
	}

	return NULL;
}

static void message_from_vector(struct mpipe_ipc_message *message,
				const struct mpipe_ipc_vector *vector)
{
	memset(message, 0, sizeof(*message));
	message->header.cmd = vector->cmd;
	message->header.generation = vector->generation;
	message->payload = (vector->payload_length != 0U)
				   ? vector->frame + MPIPE_IPC_HEADER_LENGTH
				   : NULL;
	message->payload_length = vector->payload_length;
}

/* ------------------------------------------------------------------ */
/* Shape                                                               */
/* ------------------------------------------------------------------ */

/*
 * The header is 12 bytes on purpose. SOF ships 8 in production on this same
 * HiFi4 over this same transport and NXP SRTM ships 10; the one field neither
 * needs is the boot generation, because in both of those designs the
 * lifecycle authority and the data-plane peer are the same entity.
 */
ZTEST(mpipe_ipc_protocol, test_header_is_twelve_bytes)
{
	zassert_equal(MPIPE_IPC_HEADER_LENGTH, 12U);
	zassert_equal(MPIPE_IPC_OFF_SIZE, 0U);
	zassert_equal(MPIPE_IPC_OFF_CMD, 4U);
	zassert_equal(MPIPE_IPC_OFF_GENERATION, 8U);
}

ZTEST(mpipe_ipc_protocol, test_cmd_packs_type_and_id)
{
	uint32_t cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_CONFIG, 0xbeef);

	zassert_equal(MPIPE_IPC_CMD_TYPE(cmd), MPIPE_IPC_TYPE_CONFIG);
	zassert_equal(MPIPE_IPC_CMD_ID(cmd), 0xbeef);
}

/* ------------------------------------------------------------------ */
/* Golden vectors                                                      */
/* ------------------------------------------------------------------ */

ZTEST(mpipe_ipc_protocol, test_every_golden_frame_encodes_to_exact_bytes)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		const struct mpipe_ipc_vector *vector = &mpipe_ipc_vectors[i];
		struct mpipe_ipc_message message;
		size_t written = 0;

		message_from_vector(&message, vector);

		zassert_ok(mpipe_ipc_encode(scratch, sizeof(scratch), &message, &written),
			   "vector %s failed to encode", vector->name);
		zassert_equal(written, vector->frame_length, "vector %s length",
			      vector->name);
		zassert_mem_equal(scratch, vector->frame, vector->frame_length,
				  "vector %s differs from the golden frame",
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

		zassert_equal(message.header.size, vector->size, "%s size", vector->name);
		zassert_equal(message.header.cmd, vector->cmd, "%s cmd", vector->name);
		zassert_equal(message.header.generation, vector->generation,
			      "%s generation", vector->name);
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

ZTEST(mpipe_ipc_protocol, test_encode_then_decode_round_trips_every_vector)
{
	for (size_t i = 0; i < MPIPE_IPC_VECTOR_COUNT; i++) {
		const struct mpipe_ipc_vector *vector = &mpipe_ipc_vectors[i];
		struct mpipe_ipc_message in;
		struct mpipe_ipc_message out;
		size_t written = 0;

		message_from_vector(&in, vector);
		zassert_ok(mpipe_ipc_encode(scratch, sizeof(scratch), &in, &written));
		zassert_ok(mpipe_ipc_decode(&out, scratch, written));

		zassert_equal(out.header.cmd, in.header.cmd, "%s", vector->name);
		zassert_equal(out.header.generation, in.header.generation, "%s",
			      vector->name);
		zassert_equal(out.payload_length, in.payload_length, "%s", vector->name);
	}
}

/*
 * A header-only message is legal and is the smallest thing on the wire.
 */
ZTEST(mpipe_ipc_protocol, test_header_only_message_is_legal)
{
	const struct mpipe_ipc_vector *vector = vector_named("bare_heartbeat");
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_equal(vector->frame_length, MPIPE_IPC_HEADER_LENGTH);

	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));
	zassert_equal(message.payload_length, 0U);
	zassert_is_null(message.payload, "no payload means no pointer");
}

/*
 * The format must not depend on the alignment of the receive buffer. A
 * packed-struct cast would fault or misread here on a strict-alignment target.
 */
ZTEST(mpipe_ipc_protocol, test_decode_accepts_an_unaligned_buffer)
{
	const struct mpipe_ipc_vector *vector = vector_named("status_report");
	static uint8_t staging[MPIPE_IPC_MAX_MESSAGE + 3];
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	memcpy(&staging[1], vector->frame, vector->frame_length);

	zassert_ok(mpipe_ipc_decode(&message, &staging[1], vector->frame_length));
	zassert_equal(message.header.generation, vector->generation);
}

/* ------------------------------------------------------------------ */
/* Audio format negotiation                                            */
/* ------------------------------------------------------------------ */

ZTEST(mpipe_ipc_protocol, test_config_carries_the_v1_audio_format)
{
	const struct mpipe_ipc_vector *vector = vector_named("config_request");
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));
	zassert_equal(message.payload_length, MPIPE_IPC_AUDIO_FORMAT_LENGTH);
	zassert_ok(mpipe_ipc_validate_audio(&message, &expected));
}

ZTEST(mpipe_ipc_protocol, test_audio_format_encode_round_trips)
{
	const struct mpipe_ipc_audio_format in = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;
	uint8_t buf[MPIPE_IPC_AUDIO_FORMAT_LENGTH];

	zassert_equal(mpipe_ipc_audio_format_encode(buf, sizeof(buf), &in),
		      MPIPE_IPC_AUDIO_FORMAT_LENGTH);

	message.header.cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_CONFIG, 1);
	message.payload = buf;
	message.payload_length = sizeof(buf);

	zassert_ok(mpipe_ipc_validate_audio(&message, &in));
}

/* The slow-consumer policy is part of the negotiated format, not a window. */
ZTEST(mpipe_ipc_protocol, test_xrun_policy_is_negotiated_and_compared)
{
	struct mpipe_ipc_audio_format in = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;
	uint8_t buf[MPIPE_IPC_AUDIO_FORMAT_LENGTH];

	zassert_true(expected.overrun_permitted, "v1 permits overrun by default");
	zassert_false(expected.underrun_permitted);

	in.overrun_permitted = false;
	zassert_equal(mpipe_ipc_audio_format_encode(buf, sizeof(buf), &in),
		      MPIPE_IPC_AUDIO_FORMAT_LENGTH);

	message.header.cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_CONFIG, 1);
	message.payload = buf;
	message.payload_length = sizeof(buf);

	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -EPROTO,
		      "a differing xrun policy must be rejected");
}

ZTEST(mpipe_ipc_protocol, test_audio_validation_rejects_a_non_config_message)
{
	const struct mpipe_ipc_vector *vector = vector_named("heartbeat_request");
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	zassert_ok(mpipe_ipc_decode(&message, vector->frame, vector->frame_length));
	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -ENOTSUP);
}

/* ------------------------------------------------------------------ */
/* Rejection table                                                     */
/* ------------------------------------------------------------------ */

enum mutate_kind {
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
	{ .name = "size below a full header", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_SIZE, .value = 4U, .expected = -EMSGSIZE },
	{ .name = "size beyond the control ceiling", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_SIZE, .value = MPIPE_IPC_MAX_MESSAGE + 1U,
	  .expected = -EMSGSIZE },
	{ .name = "size overflows the addition", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_SIZE, .value = 0xffffffffU, .expected = -EMSGSIZE },
	{ .name = "size larger than the bytes present", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_SIZE, .value = 200U, .expected = -EMSGSIZE },
	{ .name = "unknown message type", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_CMD, .value = MPIPE_IPC_CMD(0x7f, 1),
	  .expected = -ENOTSUP },
	{ .name = "zero generation", .kind = MUTATE_LE32,
	  .offset = MPIPE_IPC_OFF_GENERATION, .value = 0U, .expected = -EPROTO },
	{ .name = "truncated below a full header", .kind = MUTATE_TRUNCATE,
	  .length_override = MPIPE_IPC_HEADER_LENGTH - 1U, .expected = -EMSGSIZE },
	{ .name = "truncated payload", .kind = MUTATE_TRUNCATE,
	  .length_override = MPIPE_IPC_HEADER_LENGTH + 1U, .expected = -EMSGSIZE },
};

ZTEST(mpipe_ipc_protocol, test_malformed_messages_are_rejected)
{
	const struct mpipe_ipc_vector *vector = vector_named("status_report");

	zassert_not_null(vector);

	for (size_t i = 0; i < ARRAY_SIZE(reject_cases); i++) {
		const struct reject_case *test = &reject_cases[i];
		struct mpipe_ipc_message message;
		size_t length = vector->frame_length;

		memcpy(scratch, vector->frame, vector->frame_length);

		switch (test->kind) {
		case MUTATE_LE32:
			sys_put_le32(test->value, &scratch[test->offset]);
			break;
		case MUTATE_TRUNCATE:
			length = test->length_override;
			break;
		default:
			break;
		}

		zassert_equal(mpipe_ipc_decode(&message, scratch, length),
			      test->expected,
			      "case %zu (%s) returned the wrong rejection", i,
			      test->name);
	}
}

/* A rejected message must never leave a payload pointer a caller could follow. */
ZTEST(mpipe_ipc_protocol, test_rejected_message_leaves_no_payload_pointer)
{
	const struct mpipe_ipc_vector *vector = vector_named("config_request");
	struct mpipe_ipc_message message;

	zassert_not_null(vector);
	memcpy(scratch, vector->frame, vector->frame_length);
	sys_put_le32(0U, &scratch[MPIPE_IPC_OFF_GENERATION]);

	message.payload = (const uint8_t *)0x1;
	message.payload_length = 12345U;

	zassert_equal(mpipe_ipc_decode(&message, scratch, vector->frame_length),
		      -EPROTO);
	zassert_is_null(message.payload, "payload must not be exposed on rejection");
	zassert_equal(message.payload_length, 0U);
}

ZTEST(mpipe_ipc_protocol, test_encode_argument_and_capacity_rules)
{
	const struct mpipe_ipc_vector *vector = vector_named("heartbeat_request");
	struct mpipe_ipc_message message;
	size_t written = 0;

	zassert_not_null(vector);
	message_from_vector(&message, vector);

	zassert_equal(mpipe_ipc_encode(NULL, sizeof(scratch), &message, &written),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), NULL, &written),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), &message, NULL),
		      -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, vector->frame_length - 1U, &message,
				       &written),
		      -ENOSPC);

	message.payload_length = MPIPE_IPC_MAX_PAYLOAD + 1U;
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), &message, &written),
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
