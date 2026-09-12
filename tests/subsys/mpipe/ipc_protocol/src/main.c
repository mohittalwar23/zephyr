/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_session.h>

ZTEST_SUITE(mpipe_ipc, NULL, NULL, NULL, NULL, NULL);

static uint8_t scratch[MPIPE_IPC_MAX_MESSAGE];

/* ------------------------------------------------------------------ */
/* Control header                                                      */
/* ------------------------------------------------------------------ */

/*
 * Four bytes on purpose. ipc_service's receive callback supplies the length
 * and its endpoints are named, so neither a size nor a stream id earns a
 * place; peer identity lives in the session word, not on each message.
 */
ZTEST(mpipe_ipc, test_header_is_four_bytes)
{
	zassert_equal(MPIPE_IPC_HEADER_LENGTH, 4U);
	zassert_equal(MPIPE_IPC_OFF_CMD, 0U);
}

ZTEST(mpipe_ipc, test_cmd_packs_type_and_transaction_id)
{
	uint32_t cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_CONFIG, 0xbeef);

	zassert_equal(MPIPE_IPC_CMD_TYPE(cmd), MPIPE_IPC_TYPE_CONFIG);
	zassert_equal(MPIPE_IPC_CMD_ID(cmd), 0xbeef);
}

ZTEST(mpipe_ipc, test_encode_is_little_endian_and_round_trips)
{
	struct mpipe_ipc_message in = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_HELLO, 0x1234) },
		.payload = (const uint8_t *)"abcd",
		.payload_length = 4U,
	};
	struct mpipe_ipc_message out;
	size_t written = 0;

	zassert_ok(mpipe_ipc_encode(scratch, sizeof(scratch), &in, &written));
	zassert_equal(written, MPIPE_IPC_HEADER_LENGTH + 4U);
	zassert_equal(sys_get_le32(&scratch[MPIPE_IPC_OFF_CMD]), in.header.cmd);
	zassert_equal(scratch[0], 0x34, "transaction id is little-endian");

	zassert_ok(mpipe_ipc_decode(&out, scratch, written));
	zassert_equal(out.header.cmd, in.header.cmd);
	zassert_equal(out.payload_length, 4U);
	zassert_mem_equal(out.payload, "abcd", 4);
}

ZTEST(mpipe_ipc, test_header_only_message_is_legal)
{
	struct mpipe_ipc_message in = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_HEARTBEAT, 1) },
	};
	struct mpipe_ipc_message out;
	size_t written = 0;

	zassert_ok(mpipe_ipc_encode(scratch, sizeof(scratch), &in, &written));
	zassert_equal(written, MPIPE_IPC_HEADER_LENGTH);
	zassert_ok(mpipe_ipc_decode(&out, scratch, written));
	zassert_equal(out.payload_length, 0U);
	zassert_is_null(out.payload, "no payload means no pointer");
}

/* The format must not depend on receive-buffer alignment. */
ZTEST(mpipe_ipc, test_decode_accepts_an_unaligned_buffer)
{
	static uint8_t staging[MPIPE_IPC_MAX_MESSAGE + 3];
	struct mpipe_ipc_message in = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_STATUS, 7) },
		.payload = (const uint8_t *)"xy",
		.payload_length = 2U,
	};
	struct mpipe_ipc_message out;
	size_t written = 0;

	zassert_ok(mpipe_ipc_encode(scratch, sizeof(scratch), &in, &written));
	memcpy(&staging[1], scratch, written);

	zassert_ok(mpipe_ipc_decode(&out, &staging[1], written));
	zassert_equal(out.header.cmd, in.header.cmd);
}

ZTEST(mpipe_ipc, test_malformed_messages_are_rejected)
{
	struct mpipe_ipc_message out;

	zassert_equal(mpipe_ipc_decode(NULL, scratch, 8U), -EINVAL);
	zassert_equal(mpipe_ipc_decode(&out, NULL, 8U), -EINVAL);

	/* Shorter than a header. */
	zassert_equal(mpipe_ipc_decode(&out, scratch, MPIPE_IPC_HEADER_LENGTH - 1U),
		      -EMSGSIZE);

	/* Unknown type. */
	sys_put_le32(MPIPE_IPC_CMD(0x7f, 1), &scratch[MPIPE_IPC_OFF_CMD]);
	zassert_equal(mpipe_ipc_decode(&out, scratch, MPIPE_IPC_HEADER_LENGTH),
		      -ENOTSUP);

	/* Longer than the control ceiling. */
	sys_put_le32(MPIPE_IPC_CMD(MPIPE_IPC_TYPE_STATUS, 1), &scratch[MPIPE_IPC_OFF_CMD]);
	zassert_equal(mpipe_ipc_decode(&out, scratch, MPIPE_IPC_MAX_MESSAGE + 1U),
		      -EMSGSIZE);
}

ZTEST(mpipe_ipc, test_rejected_message_leaves_no_payload_pointer)
{
	struct mpipe_ipc_message out;

	sys_put_le32(MPIPE_IPC_CMD(0x7f, 1), &scratch[MPIPE_IPC_OFF_CMD]);
	out.payload = (const uint8_t *)0x1;
	out.payload_length = 12345U;

	zassert_equal(mpipe_ipc_decode(&out, scratch, MPIPE_IPC_HEADER_LENGTH), -ENOTSUP);
	zassert_is_null(out.payload);
	zassert_equal(out.payload_length, 0U);
}

ZTEST(mpipe_ipc, test_encode_argument_and_capacity_rules)
{
	struct mpipe_ipc_message in = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_HEARTBEAT, 1) },
	};
	size_t written = 0;

	zassert_equal(mpipe_ipc_encode(NULL, sizeof(scratch), &in, &written), -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), NULL, &written), -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), &in, NULL), -EINVAL);
	zassert_equal(mpipe_ipc_encode(scratch, 1U, &in, &written), -ENOSPC);

	in.payload = scratch;
	in.payload_length = MPIPE_IPC_MAX_PAYLOAD + 1U;
	zassert_equal(mpipe_ipc_encode(scratch, sizeof(scratch), &in, &written), -EMSGSIZE);
}

/* ------------------------------------------------------------------ */
/* Audio format negotiation                                            */
/* ------------------------------------------------------------------ */

ZTEST(mpipe_ipc, test_audio_format_round_trips_through_config)
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

/* The slow-consumer policy is negotiated, not a credit window. */
ZTEST(mpipe_ipc, test_xrun_policy_is_part_of_the_negotiated_format)
{
	struct mpipe_ipc_audio_format in = MPIPE_IPC_V1_AUDIO_FORMAT;
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message;
	uint8_t buf[MPIPE_IPC_AUDIO_FORMAT_LENGTH];

	zassert_true(expected.overrun_permitted, "v1 permits overrun");
	zassert_false(expected.underrun_permitted);

	in.overrun_permitted = false;
	zassert_equal(mpipe_ipc_audio_format_encode(buf, sizeof(buf), &in),
		      MPIPE_IPC_AUDIO_FORMAT_LENGTH);

	message.header.cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_CONFIG, 1);
	message.payload = buf;
	message.payload_length = sizeof(buf);

	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -EPROTO);
}

ZTEST(mpipe_ipc, test_audio_validation_rejects_a_non_config_message)
{
	const struct mpipe_ipc_audio_format expected = MPIPE_IPC_V1_AUDIO_FORMAT;
	struct mpipe_ipc_message message = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_HEARTBEAT, 1) },
	};

	zassert_equal(mpipe_ipc_validate_audio(&message, &expected), -ENOTSUP);
}

/* ------------------------------------------------------------------ */
/* Session handshake                                                   */
/* ------------------------------------------------------------------ */

ZTEST(mpipe_ipc, test_handshake_word_packs_request_and_ack)
{
	uint32_t word = MPIPE_IPC_HANDSHAKE(0x1234, 0xabcd);

	zassert_equal(MPIPE_IPC_HANDSHAKE_REQ(word), 0x1234);
	zassert_equal(MPIPE_IPC_HANDSHAKE_ACK(word), 0xabcd);
}

/*
 * The new session is derived from the value read back out of shared memory.
 * A core that started from local state would pick the same number every boot
 * and be indistinguishable from a core that never restarted.
 */
ZTEST(mpipe_ipc, test_next_session_increments_from_the_retained_value)
{
	zassert_equal(mpipe_ipc_session_next(5U, 99U), 6U);
	zassert_equal(mpipe_ipc_session_next(41U, 99U), 42U);
}

ZTEST(mpipe_ipc, test_next_session_skips_zero_on_wrap)
{
	zassert_equal(mpipe_ipc_session_next(0xffffU, 99U), 1U,
		      "zero is reserved for no session");
}

ZTEST(mpipe_ipc, test_next_session_skips_the_value_the_peer_acknowledged)
{
	zassert_equal(mpipe_ipc_session_next(7U, 8U), 9U,
		      "a fresh session must not collide with the acknowledged one");
}

ZTEST(mpipe_ipc, test_open_latch_and_steady_state)
{
	struct mpipe_ipc_session s;
	uint32_t peer;

	zassert_ok(mpipe_ipc_session_open(&s, MPIPE_IPC_HANDSHAKE(4U, 0U), 0U));
	zassert_equal(s.local_sid, 5U);
	zassert_false(s.connected);

	/* Nothing may be checked before a peer session is latched. */
	zassert_equal(mpipe_ipc_session_check(&s, MPIPE_IPC_HANDSHAKE(77U, 0U)),
		      -ENOTCONN);

	peer = MPIPE_IPC_HANDSHAKE(77U, 5U);
	zassert_ok(mpipe_ipc_session_latch(&s, peer));
	zassert_true(s.connected);
	zassert_equal(s.remote_sid, 77U);
	zassert_true(mpipe_ipc_session_acknowledged(&s, peer),
		     "the peer echoed our session, so the handshake is complete");

	zassert_ok(mpipe_ipc_session_check(&s, peer));
	zassert_equal(MPIPE_IPC_HANDSHAKE_REQ(mpipe_ipc_session_word(&s)), 5U);
	zassert_equal(MPIPE_IPC_HANDSHAKE_ACK(mpipe_ipc_session_word(&s)), 77U);
}

ZTEST(mpipe_ipc, test_latch_waits_for_a_published_peer_session)
{
	struct mpipe_ipc_session s;

	zassert_ok(mpipe_ipc_session_open(&s, 0U, 0U));
	zassert_equal(mpipe_ipc_session_latch(&s, MPIPE_IPC_HANDSHAKE(0U, 0U)), -EAGAIN);
	zassert_false(s.connected);
}

/* The case the whole mechanism exists for. */
ZTEST(mpipe_ipc, test_restarted_peer_is_detected_and_disconnects)
{
	struct mpipe_ipc_session s;

	zassert_ok(mpipe_ipc_session_open(&s, 0U, 0U));
	zassert_ok(mpipe_ipc_session_latch(&s, MPIPE_IPC_HANDSHAKE(77U, 1U)));
	zassert_ok(mpipe_ipc_session_check(&s, MPIPE_IPC_HANDSHAKE(77U, 1U)));

	/* The peer reboots and publishes a new session. */
	zassert_equal(mpipe_ipc_session_check(&s, MPIPE_IPC_HANDSHAKE(78U, 0U)),
		      -ECONNRESET);
	zassert_false(s.connected, "a restarted peer drops the session");
	zassert_equal(s.remote_sid, MPIPE_IPC_SID_NONE);

	/* And stays dropped until deliberately re-latched. */
	zassert_equal(mpipe_ipc_session_check(&s, MPIPE_IPC_HANDSHAKE(78U, 0U)),
		      -ENOTCONN);
}

ZTEST(mpipe_ipc, test_acknowledgement_is_one_way_until_the_peer_echoes)
{
	struct mpipe_ipc_session s;

	zassert_ok(mpipe_ipc_session_open(&s, 0U, 0U));
	zassert_ok(mpipe_ipc_session_latch(&s, MPIPE_IPC_HANDSHAKE(77U, 0U)));

	zassert_false(mpipe_ipc_session_acknowledged(&s, MPIPE_IPC_HANDSHAKE(77U, 0U)),
		      "peer has not echoed our session yet");
	zassert_true(mpipe_ipc_session_acknowledged(&s,
						    MPIPE_IPC_HANDSHAKE(77U, s.local_sid)));
}

ZTEST(mpipe_ipc, test_session_argument_rules)
{
	zassert_equal(mpipe_ipc_session_open(NULL, 0U, 0U), -EINVAL);
	zassert_equal(mpipe_ipc_session_latch(NULL, 0U), -EINVAL);
	zassert_equal(mpipe_ipc_session_check(NULL, 0U), -EINVAL);
	zassert_false(mpipe_ipc_session_acknowledged(NULL, 0U));
}
