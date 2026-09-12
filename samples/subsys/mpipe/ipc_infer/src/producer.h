/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* What the host side of the sample needs from whichever audio source it has. */

#ifndef MPIPE_IPC_INFER_PRODUCER_H_
#define MPIPE_IPC_INFER_PRODUCER_H_

#include <stdint.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_ring.h>

/** Start producing into @p ring. Called once the ring is up. */
int producer_start(struct mpipe_ipc_ring *ring);

/** Periods the peer could not keep up with. */
uint32_t producer_dropped(void);

#endif /* MPIPE_IPC_INFER_PRODUCER_H_ */
