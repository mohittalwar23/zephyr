/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Turning a local pointer into one the peer can use.
 *
 * Both are weak: a board where the two cores see the shared window at
 * different addresses, or through an MMU, replaces them. On a pair of cores
 * that address the same physical window identically -- the i.MX8MP M7 and
 * HiFi4 do -- the translation is the identity, and the weak definitions are
 * what make that the default rather than a special case.
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_plugin.h>

#if defined(CONFIG_MMU)
int arch_page_phys_get(void *virt, uintptr_t *phys);
#endif

__weak uintptr_t mpipe_ipc_virt_to_phys(void *virt_addr)
{
#if defined(CONFIG_MMU)
	uintptr_t phys_addr = 0;

	if (arch_page_phys_get(virt_addr, &phys_addr) == 0) {
		return phys_addr;
	}
#endif
	return (uintptr_t)virt_addr;
}

__weak void *mpipe_ipc_phys_to_virt(uintptr_t phys_addr)
{
	return (void *)phys_addr;
}
