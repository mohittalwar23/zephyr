/*
 * Copyright 2026 NXP
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

/* ------------------------------------------------------------------ */
/* Rendezvous barrier                                                  */
/* ------------------------------------------------------------------ */

#define HOST   true
#define REMOTE false
#define EXPECTED_FAULT_STATE 3U

/*
 * A published state word carries the session that published it, so a test that
 * acts as one incarnation stamps the state it publishes with that incarnation's
 * own session -- which is what a real peer's publish() writes. The cases that
 * deliberately mismatch the two stamp the word themselves.
 */
static uint32_t published(uint32_t peer_session_word, uint32_t state)
{
	return MPIPE_IPC_STATE_WORD(MPIPE_IPC_HANDSHAKE_REQ(peer_session_word), state);
}

#define STEP(session, is_host, require_peer, word, state)                                          \
	mpipe_ipc_bringup_step((session), (is_host), (require_peer), (word),                       \
			       published((word), (state)))

/* Cold boot: host may proceed with no peer at all; the remote may not. */
ZTEST(mpipe_ipc, test_cold_boot_host_opens_alone_remote_waits)
{
	struct mpipe_ipc_session h, r;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));
	zassert_ok(mpipe_ipc_session_open(&r, 0U, 0U));

	zassert_equal(STEP(&h, HOST, false, 0U, MPIPE_IPC_BRINGUP_DOWN),
		      MPIPE_IPC_ACTION_OPEN, "host owns the rings");
	zassert_equal(STEP(&r, REMOTE, false, 0U, MPIPE_IPC_BRINGUP_DOWN),
		      MPIPE_IPC_ACTION_WAIT, "remote must never build rings");
}

/* The remote attaches only once the host has finished initialising. */
ZTEST(mpipe_ipc, test_remote_waits_for_host_ready)
{
	struct mpipe_ipc_session r;
	uint32_t host_word;

	zassert_ok(mpipe_ipc_session_open(&r, 0U, 0U));

	/* A host that has not acknowledged this remote may be long gone. */
	zassert_equal(STEP(&r, REMOTE, false, MPIPE_IPC_HANDSHAKE(11U, 0U), MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_WAIT, "an unacknowledged READY may be residue");
	zassert_false(r.connected);

	host_word = MPIPE_IPC_HANDSHAKE(11U, r.local_sid);

	zassert_equal(STEP(&r, REMOTE, false, host_word, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_WAIT, "host has not built the rings yet");
	zassert_true(r.connected, "but the peer session is latched while waiting");

	zassert_equal(STEP(&r, REMOTE, false, host_word, MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_OPEN);
}

/*
 * The case that makes the barrier necessary: a restarted host must not call
 * open() while the remote is live, because open() zeroes both vrings.
 */
ZTEST(mpipe_ipc, test_restarted_host_must_not_wipe_a_live_remote)
{
	struct mpipe_ipc_session h;
	uint32_t remote_word = MPIPE_IPC_HANDSHAKE(77U, 0U);

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));

	zassert_equal(STEP(&h, HOST, false, remote_word, MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_WAIT,
		      "host must stand off while the remote is using the rings");

	/* The remote notices the new host session, tears down, and stands by. */
	zassert_equal(STEP(&h, HOST, false, remote_word, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN);
}

/* The mirror direction must also converge rather than deadlock. */
ZTEST(mpipe_ipc, test_restarted_remote_converges)
{
	struct mpipe_ipc_session h, r;
	uint32_t remote_v1, remote_v2;
	uint32_t host_word;

	/* Steady state: both up, with the remote acknowledging this host boot. */
	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));
	remote_v1 = MPIPE_IPC_HANDSHAKE(77U, h.local_sid);
	remote_v2 = MPIPE_IPC_HANDSHAKE(78U, h.local_sid);

	zassert_equal(STEP(&h, HOST, false, remote_v1, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN);
	zassert_true(h.connected);
	host_word = mpipe_ipc_session_word(&h);

	/* The remote reboots and publishes a new session. */
	zassert_equal(STEP(&h, HOST, false, remote_v2, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_FAULT, "host sees a different incarnation");
	zassert_false(h.connected);

	/* Host tears down, re-opens a session, and both converge. */
	zassert_ok(mpipe_ipc_session_open(&h, host_word, 0U));
	zassert_equal(STEP(&h, HOST, false, remote_v2, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN);

	/*
	 * The remote's own session is 78 here, so the host's published
	 * acknowledgement of 78 is what lets the remote accept it.
	 */
	zassert_ok(mpipe_ipc_session_open(&r, MPIPE_IPC_HANDSHAKE(77U, 0U), 0U));
	zassert_equal(r.local_sid, 78U);
	zassert_equal(STEP(&r, REMOTE, false, mpipe_ipc_session_word(&h), MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_OPEN, "remote attaches to the rebuilt rings");
}

/* A peer that claims READY without a session is impossible and is distrusted. */
ZTEST(mpipe_ipc, test_ready_without_a_session_is_rejected)
{
	struct mpipe_ipc_session s;

	zassert_ok(mpipe_ipc_session_open(&s, 0U, 0U));
	zassert_equal(STEP(&s, REMOTE, false, 0U, MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_FAULT);
	zassert_equal(STEP(&s, HOST, false, 0U, MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_FAULT);
}

/* A faulted peer may still own the rings, so neither role may open against it. */
ZTEST(mpipe_ipc, test_fault_and_unknown_peer_states_never_authorize_open)
{
	struct mpipe_ipc_session h;
	uint32_t peer_word;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));
	peer_word = MPIPE_IPC_HANDSHAKE(9U, h.local_sid);

	zassert_equal(STEP(&h, HOST, false, peer_word, EXPECTED_FAULT_STATE),
		      MPIPE_IPC_ACTION_FAULT);
	zassert_false(h.connected, "a faulted peer must not be latched");
	zassert_equal(STEP(&h, REMOTE, false, peer_word, EXPECTED_FAULT_STATE),
		      MPIPE_IPC_ACTION_FAULT);
	zassert_equal(STEP(&h, HOST, false, peer_word, UINT32_MAX),
		      MPIPE_IPC_ACTION_FAULT);
}

/* A peer that disappears after being latched is a fault, not a cold start. */
ZTEST(mpipe_ipc, test_peer_vanishing_after_latch_faults)
{
	struct mpipe_ipc_session s;

	zassert_ok(mpipe_ipc_session_open(&s, 0U, 0U));
	zassert_equal(STEP(&s, HOST, false, MPIPE_IPC_HANDSHAKE(9U, s.local_sid),
			   MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN);
	zassert_true(s.connected);

	zassert_equal(STEP(&s, HOST, false, 0U, MPIPE_IPC_BRINGUP_DOWN),
		      MPIPE_IPC_ACTION_FAULT);
	zassert_false(s.connected);
}

ZTEST(mpipe_ipc, test_bringup_rejects_null)
{
	zassert_equal(STEP(NULL, HOST, false, 0U, 0U),
		      MPIPE_IPC_ACTION_FAULT);
}

/*
 * The acknowledgement half is what separates a live peer from the word a dead
 * one left behind, so a session that a peer could already have acknowledged
 * must never be reissued.
 */
ZTEST(mpipe_ipc, test_an_unacknowledged_session_is_not_accepted)
{
	struct mpipe_ipc_session h;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));

	zassert_equal(STEP(&h, HOST, false, MPIPE_IPC_HANDSHAKE(3U, 0U), MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN, "residue must not block the host");
	zassert_false(h.connected, "and must not be mistaken for a peer");

	/* But it is still echoed, which is how a real peer learns we are here. */
	zassert_equal(MPIPE_IPC_HANDSHAKE_ACK(mpipe_ipc_session_word(&h)), 3U);
}

/*
 * Where the mailbox belongs to the peer's power domain, a host that opens
 * before the peer exists configures hardware that is not clocked. The writes
 * are discarded silently, so bring-up appears to succeed and the link then runs
 * one way forever. On the i.MX8MP that cost every doorbell into the host.
 */
ZTEST(mpipe_ipc, test_a_host_that_needs_its_peer_waits_for_it)
{
	struct mpipe_ipc_session h;
	uint32_t unacknowledged = MPIPE_IPC_HANDSHAKE(7U, 0U);
	uint32_t acknowledged;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));
	acknowledged = MPIPE_IPC_HANDSHAKE(7U, h.local_sid);

	/* Without the peer's acknowledgement there is no proof it is powered. */
	zassert_equal(STEP(&h, HOST, true, 0U, MPIPE_IPC_BRINGUP_DOWN),
		      MPIPE_IPC_ACTION_WAIT, "no peer at all");
	zassert_equal(STEP(&h, HOST, true, unacknowledged, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_WAIT, "a word, but nobody behind it");

	/* The same peer word, now acknowledging this boot, is proof enough. */
	zassert_equal(STEP(&h, HOST, true, acknowledged, MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN);

	/* And the default is unchanged: a host may still come up alone. */
	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));
	zassert_equal(STEP(&h, HOST, false, 0U, MPIPE_IPC_BRINGUP_DOWN),
		      MPIPE_IPC_ACTION_OPEN);
}

/*
 * The mixed sample the stamp exists for. A host that restarts publishes its new
 * session word before it publishes a state to go with it, so for a moment its
 * block holds a new session next to the READY its previous life left behind.
 * Read as a pair, those two words invite the remote into rings the new host has
 * not built.
 */
ZTEST(mpipe_ipc, test_a_ready_from_the_peers_previous_life_is_not_an_invitation)
{
	struct mpipe_ipc_session r;
	uint32_t restarted_host;

	zassert_ok(mpipe_ipc_session_open(&r, 0U, 0U));
	restarted_host = MPIPE_IPC_HANDSHAKE(9U, r.local_sid);

	zassert_equal(mpipe_ipc_bringup_step(&r, REMOTE, false, restarted_host,
					     MPIPE_IPC_STATE_WORD(5U,
								  MPIPE_IPC_BRINGUP_READY)),
		      MPIPE_IPC_ACTION_WAIT,
		      "a READY stamped with the host's previous session is not this one's");

	/* One poll later the host has stamped the state to match, and it is. */
	zassert_equal(STEP(&r, REMOTE, false, restarted_host, MPIPE_IPC_BRINGUP_READY),
		      MPIPE_IPC_ACTION_OPEN);
}

/*
 * The same disagreement seen from the host, where the conservative reading is
 * the opposite one: a state that is not this session's must not be dismissed as
 * "not READY" either, because the live half of the pair may be the state and the
 * torn half the session word -- and opening on that wipes rings a remote holds.
 */
ZTEST(mpipe_ipc, test_a_host_does_not_read_a_disagreeing_pair_as_an_idle_peer)
{
	struct mpipe_ipc_session h;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));

	zassert_equal(mpipe_ipc_bringup_step(&h, HOST, false, MPIPE_IPC_HANDSHAKE(3U, 0U),
					     MPIPE_IPC_STATE_WORD(2U,
								  MPIPE_IPC_BRINGUP_CLAIMED)),
		      MPIPE_IPC_ACTION_WAIT, "a CLAIMED from another session proves nothing");

	zassert_equal(STEP(&h, HOST, false, MPIPE_IPC_HANDSHAKE(3U, 0U),
			   MPIPE_IPC_BRINGUP_CLAIMED),
		      MPIPE_IPC_ACTION_OPEN, "once the pair agrees, an idle peer is idle");
}

/*
 * Waiting on a disagreeing pair must not become waiting forever: a peer that
 * died between its two stores leaves one behind permanently. The residue
 * timeout that already covers an unmoving READY has to cover this too.
 */
ZTEST(mpipe_ipc, test_a_pair_that_never_agrees_still_times_out_as_residue)
{
	struct mpipe_ipc_session h;
	uint32_t word = MPIPE_IPC_HANDSHAKE(3U, 0U);
	uint32_t orphaned = MPIPE_IPC_STATE_WORD(2U, MPIPE_IPC_BRINGUP_READY);
	unsigned int polls;

	zassert_ok(mpipe_ipc_session_open(&h, 0U, 0U));

	for (polls = 0; polls < MPIPE_IPC_RESIDUE_POLLS + 4U; polls++) {
		if (mpipe_ipc_bringup_step(&h, HOST, false, word, orphaned) ==
		    MPIPE_IPC_ACTION_OPEN) {
			break;
		}
	}

	zassert_true(polls >= MPIPE_IPC_RESIDUE_POLLS,
		     "the host must stand off before calling it residue");
	zassert_true(polls < MPIPE_IPC_RESIDUE_POLLS + 4U,
		     "a pair that never agrees must still time out");
}
