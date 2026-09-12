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

static const struct mpipe_ipc_ops host_ops = {
	.open_instance = host_open, .load = fake_load, .store = fake_store,
};
static const struct mpipe_ipc_ops remote_ops = {
	.open_instance = remote_open, .load = fake_load, .store = fake_store,
};

static void reset_world(void)
{
	memset(&shared_block, 0, sizeof(shared_block));
	host_opens = 0;
	remote_opens = 0;
	open_should_fail = 0;
}

/* Drive one side until it settles, so a deadlock shows up as a bounded loop. */
static int settle(struct mpipe_ipc_transport *t, int budget)
{
	int err = -EAGAIN;

	while (budget-- > 0 && err == -EAGAIN) {
		err = mpipe_ipc_transport_poll(t);
	}

	return err;
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
	zassert_ok(settle(&host, 4));
	zassert_ok(settle(&remote, 4));
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
	zassert_ok(settle(&host, 4));
	zassert_ok(settle(&remote, 4));

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
	zassert_ok(settle(&host, 4));
	zassert_ok(settle(&remote, 4));

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

	zassert_equal(mpipe_ipc_transport_poll(&host), -ECONNRESET);
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
