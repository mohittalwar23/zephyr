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
	int err;

	LOG_INF("mpipe IPC bring-up: role=%s shared=%p",
		IS_HOST ? "host" : "remote", (void *)shared);

	if (!device_is_ready(ipc)) {
		LOG_ERR("IPC device not ready");
		return -ENODEV;
	}

	err = mpipe_ipc_transport_init(&transport, shared, &mpipe_ipc_zephyr_ops,
				       (void *)ipc, IS_HOST);
	if (err != 0) {
		LOG_ERR("transport init failed: %d", err);
		return err;
	}

	LOG_INF("claimed session %u (peer word 0x%08x)", transport.session.local_sid,
		IS_HOST ? shared->remote.session : shared->host.session);

	/*
	 * Poll rather than block. This is the bounded wait static-vrings does
	 * not provide: OpenAMP's remote would otherwise spin forever inside
	 * rpmsg_virtio_wait_remote_ready() if its peer never appeared.
	 */
	do {
		err = mpipe_ipc_transport_poll(&transport);
		if (err == -EAGAIN) {
			k_msleep(20);
		}
	} while (err == -EAGAIN);

	if (err != 0) {
		LOG_ERR("bring-up faulted after %u polls: %d", transport.poll_count, err);
		return err;
	}

	LOG_INF("LINK UP after %u polls: local session %u, peer session %u",
		transport.poll_count, transport.session.local_sid,
		transport.session.remote_sid);

	/*
	 * Keep watching. A running core must notice a peer restart even when no
	 * message arrives, or a quiet link deadlocks: the restarted peer waits
	 * for this core to stand down and nothing ever prompts it to look.
	 */
	while (true) {
		k_msleep(500);

		err = mpipe_ipc_transport_poll(&transport);
		if (err == -ECONNRESET) {
			LOG_WRN("peer restarted; standing down so it can rebuild");
			(void)mpipe_ipc_transport_quiesce(&transport);
			return 0;
		}
	}

	return 0;
}
