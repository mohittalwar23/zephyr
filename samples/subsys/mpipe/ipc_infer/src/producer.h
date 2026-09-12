/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* What the host side of the sample needs from whichever audio source it has. */

#ifndef MPIPE_IPC_INFER_PRODUCER_H_
#define MPIPE_IPC_INFER_PRODUCER_H_

#include <stdint.h>

#include <zephyr/device.h>

/** Start producing. Called once the link to the peer is up. */
int producer_start(const struct device *ipc);

/** Periods the peer could not keep up with. */
uint32_t producer_dropped(void);

#endif /* MPIPE_IPC_INFER_PRODUCER_H_ */
