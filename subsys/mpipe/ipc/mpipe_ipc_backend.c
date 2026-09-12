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
 * plain volatile access is the whole contract. Were it ever made cacheable,
 * these two functions are where the invalidate and flush would belong.
 */
static uint32_t zephyr_load(const volatile uint32_t *address)
{
	return *address;
}

static void zephyr_store(volatile uint32_t *address, uint32_t value)
{
	*address = value;
}

const struct mpipe_ipc_ops mpipe_ipc_zephyr_ops = {
	.open_instance = zephyr_open_instance,
	.close_instance = zephyr_close_instance,
	.load = zephyr_load,
	.store = zephyr_store,
};
