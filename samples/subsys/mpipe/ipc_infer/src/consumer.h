/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MPIPE_IPC_INFER_CONSUMER_H_
#define MPIPE_IPC_INFER_CONSUMER_H_

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

#include "infer_sink.h"

/** Build and start the DSP-side pipeline. Called once the link is up. */
int consumer_start(const struct device *ipc, struct mpipe_ipc_transport *transport,
		   infer_result_cb on_result);

/** Stop the pipeline and deregister its audio endpoint. */
int consumer_stop(void);

/** True once the peer's sink has bound to this source. */
bool consumer_is_bound(void);

/** Publish progress into shared memory, for a core with no console. */
void consumer_publish(void);

#endif /* MPIPE_IPC_INFER_CONSUMER_H_ */
