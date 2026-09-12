/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

/* This core's own block, and the peer's, given the role. */
static volatile struct mpipe_ipc_core_block *
local_block(struct mpipe_ipc_transport *transport)
{
	return transport->is_host ? &transport->shared->host : &transport->shared->remote;
}

static volatile struct mpipe_ipc_core_block *
peer_block(struct mpipe_ipc_transport *transport)
{
	return transport->is_host ? &transport->shared->remote : &transport->shared->host;
}

static void publish(struct mpipe_ipc_transport *transport, uint32_t state)
{
	volatile struct mpipe_ipc_core_block *mine = local_block(transport);

	/*
	 * Session first, then state. A peer that samples between the two sees
	 * a new session still marked not-ready, which is a safe intermediate;
	 * the reverse order would briefly advertise readiness against a stale
	 * session.
	 */
	transport->ops->store(&mine->session, mpipe_ipc_session_word(&transport->session));
	transport->ops->store(&mine->state, state);
}

static void fault(struct mpipe_ipc_transport *transport)
{
	transport->state = MPIPE_IPC_TRANSPORT_FAULTED;
	publish(transport, MPIPE_IPC_BRINGUP_DOWN);
}

int mpipe_ipc_transport_init(struct mpipe_ipc_transport *transport,
			     volatile struct mpipe_ipc_shared *shared,
			     const struct mpipe_ipc_ops *ops, void *context,
			     bool is_host)
{
	uint32_t previous;
	uint32_t peer_word;

	if (transport == NULL || shared == NULL || ops == NULL) {
		return -EINVAL;
	}
	if (ops->open_instance == NULL || ops->load == NULL || ops->store == NULL) {
		return -EINVAL;
	}

	memset(transport, 0, sizeof(*transport));
	transport->shared = shared;
	transport->ops = ops;
	transport->context = context;
	transport->is_host = is_host;

	/*
	 * Derive the new session from what this core last published, not from
	 * local state: after a reset there is no local state, and a fixed
	 * starting value is indistinguishable from never having restarted.
	 * Reading it back is only sound because the region survives a restart,
	 * which was measured on this board rather than assumed.
	 */
	previous = ops->load(&local_block(transport)->session);
	peer_word = ops->load(&peer_block(transport)->session);

	(void)mpipe_ipc_session_open(&transport->session, previous,
				     MPIPE_IPC_HANDSHAKE_ACK(peer_word));

	transport->state = MPIPE_IPC_TRANSPORT_WAITING;
	publish(transport, MPIPE_IPC_BRINGUP_CLAIMED);

	return 0;
}

int mpipe_ipc_transport_poll(struct mpipe_ipc_transport *transport)
{
	volatile struct mpipe_ipc_core_block *theirs;
	enum mpipe_ipc_bringup_action action;
	uint32_t peer_word;
	uint32_t peer_state;
	int err;

	if (transport == NULL || transport->state == MPIPE_IPC_TRANSPORT_IDLE) {
		return -EINVAL;
	}
	if (transport->state == MPIPE_IPC_TRANSPORT_FAULTED) {
		return -ECONNRESET;
	}
	if (transport->state == MPIPE_IPC_TRANSPORT_RUNNING) {
		/*
		 * A running core must keep watching for a peer restart even
		 * when no message arrives. Detection only on receive would
		 * deadlock a quiet link: a restarted peer publishes its new
		 * session and waits for this core to stand down, and this core
		 * never looks because nothing was delivered to check.
		 */
		peer_word = transport->ops->load(&peer_block(transport)->session);

		if (!transport->session.connected) {
			/*
			 * A host that came up alone is running without a peer,
			 * which is normal rather than a fault. Latch the peer
			 * the moment it appears, so that a later restart of
			 * that same peer is detectable.
			 */
			(void)mpipe_ipc_session_latch(&transport->session, peer_word);
			return 0;
		}

		err = mpipe_ipc_session_check(&transport->session, peer_word);
		if (err == -ECONNRESET) {
			fault(transport);
		}
		return err;
	}

	theirs = peer_block(transport);
	peer_word = transport->ops->load(&theirs->session);
	peer_state = transport->ops->load(&theirs->state);
	transport->poll_count++;

	action = mpipe_ipc_bringup_step(&transport->session, transport->is_host,
					peer_word, peer_state);

	switch (action) {
	case MPIPE_IPC_ACTION_WAIT:
		/*
		 * Re-publish so the acknowledgement half tracks whatever the
		 * peer is currently advertising. This is what lets the peer
		 * learn that its restart was noticed.
		 */
		publish(transport, MPIPE_IPC_BRINGUP_CLAIMED);
		return -EAGAIN;

	case MPIPE_IPC_ACTION_OPEN:
		err = transport->ops->open_instance(transport->context);
		if (err != 0) {
			fault(transport);
			return -ECONNRESET;
		}
		transport->state = MPIPE_IPC_TRANSPORT_RUNNING;
		publish(transport, MPIPE_IPC_BRINGUP_READY);
		return 0;

	case MPIPE_IPC_ACTION_FAULT:
	default:
		fault(transport);
		return -ECONNRESET;
	}
}

int mpipe_ipc_transport_check_peer(struct mpipe_ipc_transport *transport)
{
	uint32_t peer_word;
	int err;

	if (transport == NULL) {
		return -EINVAL;
	}

	peer_word = transport->ops->load(&peer_block(transport)->session);
	err = mpipe_ipc_session_check(&transport->session, peer_word);
	if (err == -ECONNRESET) {
		fault(transport);
	}

	return err;
}

int mpipe_ipc_transport_quiesce(struct mpipe_ipc_transport *transport)
{
	if (transport == NULL) {
		return -EINVAL;
	}

	if (transport->state == MPIPE_IPC_TRANSPORT_RUNNING &&
	    transport->ops->close_instance != NULL) {
		(void)transport->ops->close_instance(transport->context);
	}

	transport->session.connected = false;
	transport->session.remote_sid = MPIPE_IPC_SID_NONE;
	transport->state = MPIPE_IPC_TRANSPORT_FAULTED;
	publish(transport, MPIPE_IPC_BRINGUP_DOWN);

	return 0;
}

uint32_t mpipe_ipc_transport_local_word(const struct mpipe_ipc_transport *transport)
{
	if (transport == NULL) {
		return 0U;
	}

	return mpipe_ipc_session_word(&transport->session);
}
