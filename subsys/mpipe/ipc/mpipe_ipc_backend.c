/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The real backend behind mpipe_ipc_ops. Deliberately thin: everything worth
 * testing lives in the transport logic, which is why that logic takes these
 * operations by injection instead of calling IPC Service directly.
 */

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/barrier.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

static int zephyr_open_instance(void *context)
{
	return ipc_service_open_instance((const struct device *)context);
}

static int zephyr_close_instance(void *context)
{
	return ipc_service_close_instance((const struct device *)context);
}

/*
 * No cache maintenance: the shared window is uncached on both cores, so a
 * plain volatile access reaches memory. Were it ever made cacheable, these two
 * functions are where the invalidate and flush would belong.
 *
 * Reaching memory is not the same as being ordered against the accesses around
 * it, which is what the bring-up sequence actually depends on: the peer may
 * only act on a published state once everything that state vouches for -- the
 * session word, the failure reason, and on the host the rings themselves -- is
 * already visible to it. `volatile` orders these accesses against each other
 * and nothing else, so the barriers below are what make the load an acquire and
 * the store a release.
 */
static uint32_t zephyr_load(const volatile uint32_t *address)
{
	uint32_t value = *address;

	barrier_dmem_fence_full();

	return value;
}

static void zephyr_store(volatile uint32_t *address, uint32_t value)
{
	barrier_dmem_fence_full();
	*address = value;
}

const struct mpipe_ipc_ops mpipe_ipc_zephyr_ops = {
	.open_instance = zephyr_open_instance,
	.close_instance = zephyr_close_instance,
	.load = zephyr_load,
	.store = zephyr_store,
};
