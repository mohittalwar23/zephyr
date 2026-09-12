/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Brings up the direct M7 <-> HiFi4 link and reports what happened, so the
 * barrier can be observed on real silicon rather than only in unit tests.
 *
 * Both images are the same source. The role, the message unit and the shared
 * window all come from devicetree, so the only difference between the two
 * cores is their overlay.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

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

int main(void)
{
	const struct device *ipc = DEVICE_DT_GET(IPC_NODE);
	struct mpipe_ipc_transport transport;
	unsigned int generation = 0;
	bool joined;
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
		joined = transport.session.connected;
		do {
			k_msleep(500);
			err = mpipe_ipc_transport_poll(&transport);

			if (err == 0 && !joined && transport.session.connected) {
				joined = true;
				LOG_INF("gen %u: peer joined: session %u", generation,
					transport.session.remote_sid);
			}
		} while (err == 0);

		if (err != -ECONNRESET) {
			LOG_ERR("gen %u: link failed: %d", generation, err);
			return err;
		}

		LOG_WRN("gen %u: peer restarted; standing down so it can rebuild",
			generation);
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
