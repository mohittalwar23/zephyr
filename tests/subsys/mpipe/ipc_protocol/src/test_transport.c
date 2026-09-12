/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

ZTEST_SUITE(mpipe_ipc_transport, NULL, NULL, NULL, NULL, NULL);

/*
 * Both cores are driven against one shared block, because that is the only
 * way the barrier's real property — that the two roles converge rather than
 * deadlock — can actually be observed.
 */
static struct mpipe_ipc_shared shared_block;
static int host_opens;
static int remote_opens;
static int open_should_fail;
static int host_closes;

static uint32_t fake_load(const volatile uint32_t *address)
{
	return *address;
}

static void fake_store(volatile uint32_t *address, uint32_t value)
{
	*address = value;
}

static int host_open(void *ctx)
{
	ARG_UNUSED(ctx);
	if (open_should_fail) {
		return -EIO;
	}
	host_opens++;
	return 0;
}

static int remote_open(void *ctx)
{
	ARG_UNUSED(ctx);
	remote_opens++;
	return 0;
}

static int host_close(void *ctx)
{
	ARG_UNUSED(ctx);
	host_closes++;
	return 0;
}

static const struct mpipe_ipc_ops host_ops = {
	.open_instance = host_open, .close_instance = host_close,
	.load = fake_load, .store = fake_store,
};
static const struct mpipe_ipc_ops remote_ops = {
	.open_instance = remote_open, .load = fake_load, .store = fake_store,
};

static void reset_world(void)
{
	memset(&shared_block, 0, sizeof(shared_block));
	host_opens = 0;
	host_closes = 0;
	remote_opens = 0;
	open_should_fail = 0;
}

/*
 * Bring both sides all the way up.
 *
 * The trailing host poll is not padding. The remote can only acknowledge the
 * host after the host has published READY, so the host learns of the remote one
 * poll later than it opens -- on hardware, on its next 500 ms watch tick.
 */
static void converge(struct mpipe_ipc_transport *host, struct mpipe_ipc_transport *remote);

/* Drive one side until it settles, so a deadlock shows up as a bounded loop. */
static int settle(struct mpipe_ipc_transport *t, int budget)
{
	int err = -EAGAIN;

	while (budget-- > 0 && err == -EAGAIN) {
		err = mpipe_ipc_transport_poll(t);
	}

	return err;
}

static void converge(struct mpipe_ipc_transport *host, struct mpipe_ipc_transport *remote)
{
	zassert_ok(settle(host, 4));
	zassert_ok(settle(remote, 4));
	zassert_ok(mpipe_ipc_transport_poll(host), "host latches the remote's ack");
	zassert_true(host->session.connected);
	zassert_true(remote->session.connected);
}

ZTEST(mpipe_ipc_transport, test_control_block_is_two_padded_core_blocks)
{
	zassert_equal(sizeof(struct mpipe_ipc_shared), 256U);
	zassert_true((offsetof(struct mpipe_ipc_shared, remote) %
		      MPIPE_IPC_CORE_BLOCK_SIZE) == 0U,
		     "each core's block must start on its own padded boundary");
}

/* A fresh session must never be zero, which is reserved for "no session". */
ZTEST(mpipe_ipc_transport, test_init_claims_a_nonzero_session_and_publishes_it)
{
	struct mpipe_ipc_transport host;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));

	zassert_not_equal(host.session.local_sid, MPIPE_IPC_SID_NONE);
	zassert_equal(MPIPE_IPC_HANDSHAKE_REQ(shared_block.host.session),
		      host.session.local_sid);
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_CLAIMED,
		      "claimed, not ready: nothing has touched a ring yet");
	zassert_equal(host_opens, 0, "init must never open the instance");
}

/*
 * The property the whole mechanism rests on: the session is derived from what
 * was left in shared memory, so a restart is visible. Deriving it locally would
 * produce the same value every boot.
 */
ZTEST(mpipe_ipc_transport, test_session_is_derived_from_retained_memory)
{
	struct mpipe_ipc_transport host;
	uint16_t first;
	uint16_t second;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	first = host.session.local_sid;

	/* Re-init exactly as a reboot would, with the block left behind. */
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	second = host.session.local_sid;

	zassert_not_equal(first, second, "a reboot must not reuse its session");
}

ZTEST(mpipe_ipc_transport, test_cold_boot_both_cores_converge)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));

	/* The remote cannot proceed until the host has built the rings. */
	zassert_equal(mpipe_ipc_transport_poll(&remote), -EAGAIN);
	zassert_equal(remote_opens, 0);

	zassert_ok(settle(&host, 4));
	zassert_equal(host_opens, 1);
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_READY);

	zassert_ok(settle(&remote, 4));
	zassert_equal(remote_opens, 1);
	zassert_equal(shared_block.remote.state, MPIPE_IPC_BRINGUP_READY);
}

/*
 * The case the barrier exists for. A restarted host must not open — and so
 * must not clear both vrings — while the remote is still READY.
 */
ZTEST(mpipe_ipc_transport, test_restarted_host_waits_for_a_live_remote)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);
	zassert_equal(host_opens, 1);

	/* The host reboots. The remote is untouched and still READY. */
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_equal(settle(&host, 8), -EAGAIN, "must not open under a live remote");
	zassert_equal(host_opens, 1, "no second open happened");

	/* The remote notices the new host session and stands down. */
	zassert_equal(mpipe_ipc_transport_poll(&remote), -ECONNRESET);
	zassert_ok(mpipe_ipc_transport_quiesce(&remote));
	zassert_equal(shared_block.remote.state, MPIPE_IPC_BRINGUP_DOWN);

	/* Only now may the host rebuild. */
	zassert_ok(settle(&host, 4));
	zassert_equal(host_opens, 2);
}

/* The mirror direction must converge too, not deadlock. */
ZTEST(mpipe_ipc_transport, test_restarted_remote_converges)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);

	/* The remote reboots. */
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));

	/* The host sees a different incarnation and faults. */
	zassert_equal(mpipe_ipc_transport_poll(&host), -ECONNRESET);
	zassert_ok(mpipe_ipc_transport_quiesce(&host));

	/* Both re-init and converge. */
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(settle(&host, 4));
	zassert_ok(settle(&remote, 4));
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_READY);
	zassert_equal(shared_block.remote.state, MPIPE_IPC_BRINGUP_READY);
}

/* A peer that restarts mid-stream must be caught before its data is used. */
ZTEST(mpipe_ipc_transport, test_check_peer_faults_on_a_restarted_peer)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);

	zassert_ok(mpipe_ipc_transport_check_peer(&host), "steady state is fine");

	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	zassert_equal(mpipe_ipc_transport_check_peer(&host), -ECONNRESET);
	zassert_equal(host.state, MPIPE_IPC_TRANSPORT_FAULTED);
}

/* A backend that refuses to open must fault rather than claim readiness. */
ZTEST(mpipe_ipc_transport, test_open_failure_faults_and_does_not_publish_ready)
{
	struct mpipe_ipc_transport host;

	reset_world();
	open_should_fail = 1;
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));

	/*
	 * The backend's own error must survive. A backend that refuses to open
	 * is a local misconfiguration, not a peer restart, and the two demand
	 * opposite responses: stop and fix the build, versus keep retrying.
	 * Reporting -ECONNRESET here hides the only number that says which one
	 * happened -- as it did on the board, where an IPC Service failure
	 * surfaced on the console as a peer reset.
	 */
	zassert_equal(mpipe_ipc_transport_poll(&host), -EIO);
	zassert_equal(host_opens, 0);
	zassert_equal(host.state, MPIPE_IPC_TRANSPORT_FAULTED);
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_DOWN,
		      "a failed open must never advertise READY");
}

ZTEST(mpipe_ipc_transport, test_poll_is_idempotent_once_running)
{
	struct mpipe_ipc_transport host;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(settle(&host, 4));

	zassert_ok(mpipe_ipc_transport_poll(&host));
	zassert_ok(mpipe_ipc_transport_poll(&host));
	zassert_equal(host_opens, 1, "the instance must be opened exactly once");
}

ZTEST(mpipe_ipc_transport, test_argument_rules)
{
	struct mpipe_ipc_transport t;
	struct mpipe_ipc_ops incomplete = { .load = fake_load, .store = fake_store };

	reset_world();
	zassert_equal(mpipe_ipc_transport_init(NULL, &shared_block, &host_ops, NULL, true),
		      -EINVAL);
	zassert_equal(mpipe_ipc_transport_init(&t, NULL, &host_ops, NULL, true), -EINVAL);
	zassert_equal(mpipe_ipc_transport_init(&t, &shared_block, NULL, NULL, true),
		      -EINVAL);
	zassert_equal(mpipe_ipc_transport_init(&t, &shared_block, &incomplete, NULL, true),
		      -EINVAL, "an ops table without open_instance is unusable");
	zassert_equal(mpipe_ipc_transport_poll(NULL), -EINVAL);
	zassert_equal(mpipe_ipc_transport_quiesce(NULL), -EINVAL);
}


/*
 * The board-found bug. A core killed by the lifecycle authority leaves its
 * session word behind in retained memory, and a host booting afterwards used to
 * latch it as if a peer were live -- then read that peer's actual boot as a
 * restart and stand down, leaving the link permanently half-up.
 */
ZTEST(mpipe_ipc_transport, test_residue_from_a_dead_peer_is_not_a_live_peer)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();

	/* A previous remote lived and died, leaving its word behind. */
	shared_block.remote.session = MPIPE_IPC_HANDSHAKE(3U, 0U);
	shared_block.remote.state = MPIPE_IPC_BRINGUP_DOWN;

	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(settle(&host, 4), "residue must not stop the host opening");
	zassert_false(host.session.connected,
		      "a word nobody is behind is not a peer");

	/* The real remote now boots and claims a session of its own. */
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	zassert_ok(mpipe_ipc_transport_poll(&host),
		   "the remote's arrival is not a restart of the residue");
	zassert_ok(settle(&remote, 4));
	zassert_ok(mpipe_ipc_transport_poll(&host));

	zassert_true(host.session.connected);
	zassert_equal(host.session.remote_sid, remote.session.local_sid);
	zassert_equal(host_opens, 1, "the host never had to rebuild");
	zassert_equal(remote_opens, 1);
}

/*
 * The other half of the same problem: a core killed while READY leaves READY
 * behind. The host must stand off long enough for a live remote to answer, then
 * conclude the word is residue rather than wait forever.
 */
ZTEST(mpipe_ipc_transport, test_a_ready_word_nobody_answers_for_times_out)
{
	struct mpipe_ipc_transport host;
	int err = -EAGAIN;
	unsigned int polls = 0;

	reset_world();
	shared_block.remote.session = MPIPE_IPC_HANDSHAKE(3U, 0U);
	shared_block.remote.state = MPIPE_IPC_BRINGUP_READY;

	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));

	zassert_equal(settle(&host, MPIPE_IPC_RESIDUE_POLLS - 1), -EAGAIN,
		      "a READY peer gets the benefit of the doubt first");
	zassert_equal(host_opens, 0);

	while (err == -EAGAIN && polls < MPIPE_IPC_RESIDUE_POLLS + 4U) {
		err = mpipe_ipc_transport_poll(&host);
		polls++;
	}
	zassert_ok(err, "an unmoving unacknowledged READY peer is residue");
	zassert_equal(host_opens, 1);
}

/*
 * Standing down after a fault must still hand the instance back. Detecting a
 * peer restart leaves the transport FAULTED but the instance open, and a close
 * conditioned on RUNNING would skip exactly that case -- on the board the next
 * open then failed with -EALREADY and the link never came back.
 */
ZTEST(mpipe_ipc_transport, test_quiesce_closes_an_instance_left_open_by_a_fault)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);
	zassert_equal(host_opens, 1);

	/* The remote restarts; the host faults while its instance is open. */
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	zassert_equal(mpipe_ipc_transport_poll(&host), -ECONNRESET);
	zassert_equal(host.state, MPIPE_IPC_TRANSPORT_FAULTED);
	zassert_true(host.opened, "the fault did not close anything");

	zassert_ok(mpipe_ipc_transport_quiesce(&host));
	zassert_equal(host_closes, 1, "standing down must release the instance");
	zassert_false(host.opened);

	/* And the rebuild succeeds. */
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(settle(&host, 4));
	zassert_equal(host_opens, 2);
}

/*
 * A core that fails bring-up must say why in shared memory. Both cores here
 * share a single UART, so at most one has a console; the published reason is
 * the only diagnostic the other one has.
 */
ZTEST(mpipe_ipc_transport, test_a_failure_reason_is_published_for_the_peer)
{
	struct mpipe_ipc_transport host;

	reset_world();
	open_should_fail = 1;
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_equal(shared_block.host.error, 0, "claiming a session clears the reason");

	zassert_equal(mpipe_ipc_transport_poll(&host), -EIO);
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_DOWN);
	zassert_equal(shared_block.host.error, -EIO, "the peer can see the cause");

	/* A fresh session must not inherit the previous life's reason. */
	open_should_fail = 0;
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_equal(shared_block.host.error, 0);
	zassert_ok(settle(&host, 4));
	zassert_equal(shared_block.host.error, 0);
}

/* Standing down deliberately is not a failure, and must not read as one. */
ZTEST(mpipe_ipc_transport, test_a_clean_stand_down_publishes_no_error)
{
	struct mpipe_ipc_transport host, remote;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);

	zassert_ok(mpipe_ipc_transport_quiesce(&host));
	zassert_equal(shared_block.host.state, MPIPE_IPC_BRINGUP_DOWN);
	zassert_equal(shared_block.host.error, 0);
}

/*
 * Recovering from a peer restart must not look like a restart of this core, or
 * the two take turns tearing each other down and the link never settles. This
 * is the sequence that failed on the board.
 */
ZTEST(mpipe_ipc_transport, test_rebuilding_does_not_look_like_a_restart)
{
	struct mpipe_ipc_transport host, remote;
	uint16_t host_sid;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	converge(&host, &remote);
	host_sid = host.session.local_sid;

	/* The remote restarts; the host notices and stands down. */
	zassert_ok(mpipe_ipc_transport_init(&remote, &shared_block, &remote_ops, NULL,
					    false));
	zassert_equal(mpipe_ipc_transport_poll(&host), -ECONNRESET);
	zassert_ok(mpipe_ipc_transport_quiesce(&host));

	zassert_ok(mpipe_ipc_transport_rebuild(&host));
	zassert_equal(host.session.local_sid, host_sid,
		      "the host did not restart and must not say it did");

	/* Both converge, and the remote never sees a host restart. */
	converge(&host, &remote);
	zassert_equal(host.session.remote_sid, remote.session.local_sid);
	zassert_equal(remote.session.remote_sid, host_sid);
	zassert_ok(mpipe_ipc_transport_poll(&remote), "the remote stays up");
	zassert_equal(shared_block.remote.error, 0);
}

/* Rebuilding with the instance still open would leak it. */
ZTEST(mpipe_ipc_transport, test_rebuild_requires_the_instance_released)
{
	struct mpipe_ipc_transport host;

	reset_world();
	zassert_ok(mpipe_ipc_transport_init(&host, &shared_block, &host_ops, NULL, true));
	zassert_ok(settle(&host, 4));

	zassert_equal(mpipe_ipc_transport_rebuild(&host), -EBUSY);
	zassert_equal(mpipe_ipc_transport_rebuild(NULL), -EINVAL);

	zassert_ok(mpipe_ipc_transport_quiesce(&host));
	zassert_ok(mpipe_ipc_transport_rebuild(&host));
}
