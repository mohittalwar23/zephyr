/*
 * Copyright (c) 2026 Mohit Talwar
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
	session->remote_sid = MPIPE_IPC_SID_NONE;
	session->connected = false;

	return 0;
}

uint32_t mpipe_ipc_session_word(const struct mpipe_ipc_session *session)
{
	if (session == NULL) {
		return MPIPE_IPC_HANDSHAKE(MPIPE_IPC_SID_NONE, MPIPE_IPC_SID_NONE);
	}

	return MPIPE_IPC_HANDSHAKE(session->local_sid, session->remote_sid);
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
