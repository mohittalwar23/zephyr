/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_session.h>

uint16_t mpipe_ipc_session_next(uint16_t previous_req, uint16_t peer_ack)
{
	uint16_t candidate = previous_req;

	/*
	 * Skip the reserved zero and the value the peer has already
	 * acknowledged, so a fresh session can never be mistaken for the one
	 * that was live before this boot. At most two values are skipped, so
	 * this terminates.
	 */
	do {
		candidate = (uint16_t)((candidate + 1U) & MPIPE_IPC_SID_MASK);
	} while (candidate == MPIPE_IPC_SID_NONE || candidate == peer_ack);

	return candidate;
}

int mpipe_ipc_session_open(struct mpipe_ipc_session *session, uint32_t previous_word,
			   uint16_t peer_ack)
{
	if (session == NULL) {
		return -EINVAL;
	}

	memset(session, 0, sizeof(*session));
	session->local_sid =
		mpipe_ipc_session_next(MPIPE_IPC_HANDSHAKE_REQ(previous_word), peer_ack);
	session->peer_seen = MPIPE_IPC_SID_NONE;
	session->remote_sid = MPIPE_IPC_SID_NONE;
	session->connected = false;

	return 0;
}

uint32_t mpipe_ipc_session_word(const struct mpipe_ipc_session *session)
{
	if (session == NULL) {
		return MPIPE_IPC_HANDSHAKE(MPIPE_IPC_SID_NONE, MPIPE_IPC_SID_NONE);
	}

	/*
	 * The acknowledgement half carries what this core has seen, not what it
	 * has accepted. Publishing the accepted value instead would deadlock a
	 * cold boot: acceptance requires the peer's acknowledgement, so neither
	 * side would ever acknowledge first.
	 */
	return MPIPE_IPC_HANDSHAKE(session->local_sid, session->peer_seen);
}

int mpipe_ipc_session_latch(struct mpipe_ipc_session *session, uint32_t peer_word)
{
	uint16_t peer_req;

	if (session == NULL) {
		return -EINVAL;
	}

	peer_req = MPIPE_IPC_HANDSHAKE_REQ(peer_word);
	if (peer_req == MPIPE_IPC_SID_NONE) {
		return -EAGAIN;
	}

	session->peer_seen = peer_req;
	session->remote_sid = peer_req;
	session->connected = true;

	return 0;
}

int mpipe_ipc_session_check(struct mpipe_ipc_session *session, uint32_t peer_word)
{
	if (session == NULL) {
		return -EINVAL;
	}
	if (!session->connected) {
		return -ENOTCONN;
	}

	if (MPIPE_IPC_HANDSHAKE_REQ(peer_word) != session->remote_sid) {
		/*
		 * The peer restarted. Drop the session rather than consume a
		 * message from an incarnation we never handshook with.
		 */
		session->connected = false;
		session->remote_sid = MPIPE_IPC_SID_NONE;
		return -ECONNRESET;
	}

	return 0;
}

bool mpipe_ipc_session_acknowledged(const struct mpipe_ipc_session *session,
				    uint32_t peer_word)
{
	if (session == NULL) {
		return false;
	}

	return MPIPE_IPC_HANDSHAKE_ACK(peer_word) == session->local_sid;
}

bool mpipe_ipc_session_observe(struct mpipe_ipc_session *session, uint32_t peer_word)
{
	uint16_t peer_req = MPIPE_IPC_HANDSHAKE_REQ(peer_word);

	/* Echo unconditionally: this is the peer's only evidence we exist. */
	session->peer_seen = peer_req;

	return (peer_req != MPIPE_IPC_SID_NONE) &&
	       (MPIPE_IPC_HANDSHAKE_ACK(peer_word) == session->local_sid);
}

/* Count consecutive polls in which the peer published nothing new. */
static void track_stall(struct mpipe_ipc_session *session, uint32_t peer_word,
			uint32_t peer_state_word)
{
	if (peer_word != session->peer_last_word ||
	    peer_state_word != session->peer_last_state) {
		session->peer_last_word = peer_word;
		session->peer_last_state = peer_state_word;
		session->peer_stall = 0U;
	} else if (session->peer_stall < UINT16_MAX) {
		session->peer_stall++;
	}
}

enum mpipe_ipc_bringup_action mpipe_ipc_bringup_step(struct mpipe_ipc_session *session,
						     bool is_host, bool require_peer,
						     uint32_t peer_session_word,
						     uint32_t peer_state_word)
{
	uint32_t peer_state = MPIPE_IPC_STATE_OF(peer_state_word);
	bool state_is_current;
	uint16_t peer_req;
	bool acked;

	if (session == NULL) {
		return MPIPE_IPC_ACTION_FAULT;
	}
	if (peer_state >= MPIPE_IPC_BRINGUP_FAULT) {
		/* FAULT and unknown states cannot prove the rings are released. */
		return MPIPE_IPC_ACTION_FAULT;
	}

	peer_req = MPIPE_IPC_HANDSHAKE_REQ(peer_session_word);

	/*
	 * The two words are read separately, so a peer caught partway through
	 * republishing yields a pair that does not agree on who published it.
	 * The session word is the newer of the two -- publish() writes it first
	 * -- which leaves a state belonging to the incarnation before it. Such a
	 * state is not evidence either way: it cannot invite this core in, and
	 * it equally cannot be dismissed as "not READY", because the peer it
	 * came from may be the live one and the session word the torn half.
	 * Everything below therefore waits on it, and only the residue timeout
	 * resolves a pair that never agrees because the peer died between its
	 * two stores.
	 */
	state_is_current = MPIPE_IPC_STATE_SID(peer_state_word) == peer_req;

	/* A peer claiming READY without a session is impossible; do not trust it. */
	if (state_is_current && peer_req == MPIPE_IPC_SID_NONE &&
	    peer_state == MPIPE_IPC_BRINGUP_READY) {
		return MPIPE_IPC_ACTION_FAULT;
	}

	acked = mpipe_ipc_session_observe(session, peer_session_word);
	track_stall(session, peer_session_word, peer_state_word);

	if (session->connected) {
		if (!acked || peer_req != session->remote_sid) {
			session->connected = false;
			session->remote_sid = MPIPE_IPC_SID_NONE;
			return MPIPE_IPC_ACTION_FAULT;
		}
	} else if (acked) {
		session->remote_sid = peer_req;
		session->connected = true;
	}

	if (is_host) {
		/*
		 * Where the mailbox belongs to the peer's power domain, opening
		 * before the peer exists configures hardware that is not
		 * clocked. Nothing reports an error; the writes simply do not
		 * land, and the link then runs one-way forever.
		 */
		if (require_peer && !acked) {
			return MPIPE_IPC_ACTION_WAIT;
		}

		/*
		 * Never clear rings a live remote is reading. The wait does not
		 * require an acknowledgement, because a remote that is live but
		 * has not yet noticed this boot still holds those rings.
		 */
		if (state_is_current && peer_state != MPIPE_IPC_BRINGUP_READY) {
			return MPIPE_IPC_ACTION_OPEN;
		}
		if (!acked && session->peer_stall >= MPIPE_IPC_RESIDUE_POLLS) {
			/* Unmoving, and never acknowledged this boot: residue. */
			return MPIPE_IPC_ACTION_OPEN;
		}
		return MPIPE_IPC_ACTION_WAIT;
	}

	/*
	 * The remote attaches to rings the host built, so it needs proof the
	 * host is alive and finished -- a READY it has not been acknowledged by
	 * may be the leftover word of a host that is gone, and a READY the host
	 * no longer stands behind is the same thing one restart later.
	 */
	return (acked && state_is_current && peer_state == MPIPE_IPC_BRINGUP_READY)
		       ? MPIPE_IPC_ACTION_OPEN
		       : MPIPE_IPC_ACTION_WAIT;
}
