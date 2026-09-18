/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Brings up the direct M7 <-> HiFi4 link and reports what happened, so the
 * barrier can be observed on real silicon rather than only in unit tests.
 *
 * The link only. What travels over it once it is up -- audio, and a pipeline
 * spanning both cores -- is the ipc_infer sample; keeping a second data path
 * here only gave two ways to do the same thing.
 *
 * Both images are the same source. The role, the message unit and the shared
 * window all come from devicetree, so the only difference between the two
 * cores is their overlay.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

LOG_MODULE_REGISTER(mpipe_ipc_bringup, LOG_LEVEL_INF);

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
#include <fsl_mu.h>

/*
 * Linux's imx_dsp_rproc does not consider the DSP started until the firmware
 * signals readiness on MU2; without it `echo start` fails with a timeout and
 * the core is torn straight back down.
 *
 * This is separate from, and earlier than, the MU3 link this sample brings up:
 * MU2 is the DSP's channel to Linux, MU3 is its channel to the M7. One
 * general-purpose interrupt, sent once, is the whole protocol.
 */
static void signal_linux_ready(void)
{
	MU_Type *const mu2_b = (MU_Type *)(uintptr_t)0x30e70000U;

	(void)MU_TriggerInterrupts(mu2_b, kMU_GenInt0InterruptTrigger);
}
#else
static inline void signal_linux_ready(void)
{
}
#endif

#define IPC_NODE    DT_NODELABEL(ipc0)
#define SHARED_NODE DT_NODELABEL(mpipe_ipc_ctrl)

/*
 * The control block lives in its own reserved region, deliberately outside the
 * area IPC Service owns: the static-vrings host clears the whole vdev status
 * area on open, which would erase the very session this design retains.
 */
static volatile struct mpipe_ipc_shared *const shared =
	(volatile struct mpipe_ipc_shared *)DT_REG_ADDR(SHARED_NODE);

BUILD_ASSERT(DT_REG_SIZE(SHARED_NODE) >= sizeof(struct mpipe_ipc_shared),
	     "the reserved control region is too small for the shared block");

/*
 * Which side clears the rings is a devicetree decision, not a build-time one.
 * Read the enum index the same way the static-vrings backend itself does
 * (DT_ENUM_IDX_OR with host first), so the two can never disagree about which
 * core owns the rings.
 */
#define IS_HOST (DT_ENUM_IDX_OR(IPC_NODE, role, 0) == 0)

/*
 * Both cores register the same endpoint name; rpmsg's name service pairs them.
 * One endpoint is enough here -- this sample proves the link carries data, and
 * audio will not travel on it in any case.
 */
#define MPIPE_IPC_EP_NAME "mpipe.ctrl"

/*
 * File scope because the receive callback runs on the backend's work queue and
 * needs the transport to verify the sender before acting on anything it sent.
 */
static struct mpipe_ipc_transport transport;
static struct ipc_ept endpoint;

static K_SEM_DEFINE(endpoint_bound, 0, 1);
static K_SEM_DEFINE(reply_arrived, 0, 1);

static uint16_t expected_reply_id;
static atomic_t round_trips;
static atomic_t echoes;
static atomic_t drops;

static int send_command(uint16_t type, uint16_t id)
{
	uint8_t frame[MPIPE_IPC_HEADER_LENGTH];
	struct mpipe_ipc_message message = {
		.header = { .cmd = MPIPE_IPC_CMD(type, id) },
	};
	size_t written;
	int err;

	err = mpipe_ipc_encode(frame, sizeof(frame), &message, &written);
	if (err != 0) {
		return err;
	}

	/*
	 * ipc_service_send() reports the byte count on success, not zero.
	 * Treating a non-zero return as an error -- or passing it on as one --
	 * turns every successful send into a plausible-looking number.
	 */
	err = ipc_service_send(&endpoint, frame, written);
	if (err < 0) {
		return err;
	}
	if ((size_t)err != written) {
		return -EIO;
	}

	return 0;
}

static void on_bound(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&endpoint_bound);
}

static void on_received(const void *data, size_t length, void *priv)
{
	struct mpipe_ipc_message message;
	int err;

	ARG_UNUSED(priv);

	/*
	 * Verify the sender before decoding, as ICMsg does. This is the first
	 * point on hardware where bytes could arrive from an incarnation this
	 * core never handshook with: the rings survive a peer restart, so a
	 * message left in them is indistinguishable from a fresh one by content
	 * alone.
	 */
	err = mpipe_ipc_transport_check_peer(&transport);
	if (err != 0) {
		atomic_inc(&drops);
		LOG_WRN("dropped %zu bytes from an unverified peer: %d", length, err);
		return;
	}

	err = mpipe_ipc_decode(&message, data, length);
	if (err != 0) {
		atomic_inc(&drops);
		LOG_WRN("dropped a malformed message of %zu bytes: %d", length, err);
		return;
	}

	switch (MPIPE_IPC_CMD_TYPE(message.header.cmd)) {
	case MPIPE_IPC_TYPE_HEARTBEAT:
		/* The remote half of the round trip. */
		err = send_command(MPIPE_IPC_TYPE_HEARTBEAT_ACK,
				   MPIPE_IPC_CMD_ID(message.header.cmd));
		if (err != 0) {
			LOG_ERR("could not echo heartbeat %u: %d",
				MPIPE_IPC_CMD_ID(message.header.cmd), err);
		} else {
			atomic_inc(&echoes);
		}
		break;

	case MPIPE_IPC_TYPE_HEARTBEAT_ACK:
		if (MPIPE_IPC_CMD_ID(message.header.cmd) == expected_reply_id) {
			atomic_inc(&round_trips);
			k_sem_give(&reply_arrived);
		}
		break;

	default:
		LOG_WRN("unexpected message type 0x%04x",
			MPIPE_IPC_CMD_TYPE(message.header.cmd));
		break;
	}
}

static const struct ipc_ept_cfg endpoint_cfg = {
	.name = MPIPE_IPC_EP_NAME,
	.cb = {
		.bound = on_bound,
		.received = on_received,
	},
};

/* One request, one reply. Returns the round-trip time or a negative errno. */
static int exchange_heartbeat(uint16_t id)
{
	uint32_t started;
	int err;

	expected_reply_id = id;
	k_sem_reset(&reply_arrived);

	started = k_cycle_get_32();

	err = send_command(MPIPE_IPC_TYPE_HEARTBEAT, id);
	if (err < 0) {
		return err;
	}

	if (k_sem_take(&reply_arrived, K_MSEC(200)) != 0) {
		return -ETIMEDOUT;
	}

	return (int)k_cyc_to_us_near32(k_cycle_get_32() - started);
}

int main(void)
{
	const struct device *ipc = DEVICE_DT_GET(IPC_NODE);
	unsigned int generation = 0;
	uint16_t heartbeat_id = 0;
	bool joined;
	bool bound;
	bool announced;
	int err;

	/*
	 * Tell Linux we are alive before anything else. Its remoteproc start
	 * is blocking on this, and a timeout there stops the core before the
	 * MU3 rendezvous below ever gets a chance to run.
	 */
	signal_linux_ready();

	LOG_INF("mpipe IPC bring-up: role=%s shared=%p",
		IS_HOST ? "host" : "remote", (void *)shared);

	if (!device_is_ready(ipc)) {
		LOG_ERR("IPC device not ready");
		return -ENODEV;
	}

	/*
	 * Rebuild after a peer restart rather than exiting. Standing down is
	 * only half the contract: the peer is waiting for this core to reach
	 * DOWN so it can rebuild the rings, and then for this core to come back
	 * and meet it. A core that exited here would leave the link permanently
	 * half-up, which is exactly what a restart is supposed to recover from.
	 */
	err = mpipe_ipc_transport_init(&transport, shared, &mpipe_ipc_zephyr_ops,
				       (void *)ipc, IS_HOST);
	if (err != 0) {
		LOG_ERR("transport init failed: %d", err);
		return err;
	}

	/*
	 * MU3 lives in AUDIOMIX and is clocked as a peripheral clock of the DSP
	 * node, so it runs only while Linux holds the DSP runtime-resumed. A
	 * host that opened first would configure an unclocked mailbox: the
	 * writes vanish without an error, bring-up still succeeds, and the link
	 * then runs one way forever because the host's receive interrupts were
	 * never really enabled.
	 */
	(void)mpipe_ipc_transport_require_peer(&transport, true);

	while (true) {
		LOG_INF("gen %u: session %u (peer word 0x%08x)", generation,
			transport.session.local_sid,
			IS_HOST ? shared->remote.session : shared->host.session);

		/*
		 * Poll rather than block. This is the bounded wait
		 * static-vrings does not provide: OpenAMP's remote would
		 * otherwise spin forever inside rpmsg_virtio_wait_remote_ready()
		 * if its peer never appeared.
		 */
		do {
			err = mpipe_ipc_transport_poll(&transport);
			if (err == -EAGAIN) {
				k_msleep(20);
			}
		} while (err == -EAGAIN);

		if (err != 0) {
			LOG_ERR("gen %u: bring-up failed after %u polls: %d",
				generation, transport.poll_count, err);
			return err;
		}

		if (transport.session.connected) {
			LOG_INF("gen %u: LINK UP after %u polls: session %u <-> %u",
				generation, transport.poll_count,
				transport.session.local_sid,
				transport.session.remote_sid);
		} else {
			/*
			 * Normal for a host that opened before its peer existed.
			 * The rings are up; there is simply nobody on the far
			 * end yet, and the join is reported when it happens.
			 */
			LOG_INF("gen %u: LINK UP after %u polls: session %u, no peer yet",
				generation, transport.poll_count,
				transport.session.local_sid);
		}

		/*
		 * Keep watching. A running core must notice a peer restart even
		 * when no message arrives, or a quiet link deadlocks: the
		 * restarted peer waits for this core to stand down and nothing
		 * ever prompts it to look.
		 */
		/*
		 * Register after the rings exist, and do not block on the bind:
		 * a host that opened before its peer booted has nobody to pair
		 * with yet, and rpmsg's name service completes the pairing
		 * whenever the peer does arrive.
		 */
		k_sem_reset(&endpoint_bound);
		err = ipc_service_register_endpoint(ipc, &endpoint, &endpoint_cfg);
		if (err != 0) {
			LOG_ERR("gen %u: cannot register endpoint: %d", generation, err);
			return err;
		}

		joined = transport.session.connected;
		bound = false;
		announced = false;
		do {
			k_msleep(500);
			err = mpipe_ipc_transport_poll(&transport);

			if (err == 0 && !joined && transport.session.connected) {
				joined = true;
				LOG_INF("gen %u: peer joined: session %u", generation,
					transport.session.remote_sid);
			}

			if (err == 0 && !bound &&
			    k_sem_take(&endpoint_bound, K_NO_WAIT) == 0) {
				bound = true;
				LOG_INF("gen %u: endpoint '%s' bound", generation,
					MPIPE_IPC_EP_NAME);
			}

			/*
			 * The host drives; the remote answers from its receive
			 * callback. Only the host needs the session to be
			 * two-sided first, because only it can open without a
			 * peer present.
			 */
			if (err == 0 && IS_HOST && bound && joined) {
				int rtt = exchange_heartbeat(heartbeat_id);

				if (rtt < 0) {
					LOG_ERR("gen %u: heartbeat %u failed: %d",
						generation, heartbeat_id, rtt);
				} else if (!announced) {
					announced = true;
					LOG_INF("gen %u: FIRST ROUND TRIP: heartbeat %u "
						"acknowledged in %d us",
						generation, heartbeat_id, rtt);
				} else if ((heartbeat_id % 20U) == 0U) {
					LOG_INF("gen %u: %ld round trips, last %d us, "
						"%ld dropped",
						generation,
						(long)atomic_get(&round_trips), rtt,
						(long)atomic_get(&drops));
				}
				heartbeat_id++;
			}
		} while (err == 0);

		if (err != -ECONNRESET) {
			LOG_ERR("gen %u: link failed: %d", generation, err);
			return err;
		}

		LOG_WRN("gen %u: peer restarted; standing down so it can rebuild",
			generation);

		/*
		 * Deregister before standing down. The backend refuses to close
		 * an instance that still has a bound endpoint, and that refusal
		 * leaves the instance wedged rather than merely failing, so the
		 * order here is not a preference.
		 */
		err = ipc_service_deregister_endpoint(&endpoint);
		if (err != 0) {
			LOG_ERR("gen %u: cannot release the endpoint: %d", generation,
				err);
		}

		err = mpipe_ipc_transport_quiesce(&transport);
		if (err != 0) {
			LOG_ERR("gen %u: standing down did not release the instance: %d",
				generation, err);
		}

		/*
		 * Give the peer time to observe DOWN before claiming a new
		 * session. Coming straight back would race the peer's own
		 * detection and could be mistaken for a second restart.
		 */
		k_msleep(200);
		generation++;

		/*
		 * Rebuild rather than re-initialise. This core did not restart;
		 * claiming a new session here would tell the peer it had, and
		 * the two would take turns tearing each other down.
		 */
		err = mpipe_ipc_transport_rebuild(&transport);
		if (err != 0) {
			LOG_ERR("gen %u: cannot rebuild: %d", generation, err);
			return err;
		}
	}

	return 0;
}
