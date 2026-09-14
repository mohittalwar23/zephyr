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
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

/** Start producing. Called once the link to the peer is up. */
int producer_start(const struct device *ipc, struct mpipe_ipc_transport *transport);

/** Stop the pipeline and deregister its audio endpoint. */
int producer_stop(void);

/** Periods the peer could not keep up with. */
uint32_t producer_dropped(void);

/** Publish progress into shared memory, for a core with no usable console. */
void producer_publish(void);

#endif /* MPIPE_IPC_INFER_PRODUCER_H_ */
