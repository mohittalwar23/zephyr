/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Pipeline elements that span two cores.
 * @ingroup mpipe_ipc_plugin
 *
 * A pipeline ends on one core in an @ref mpipe_ipc_sink and resumes on the
 * other in an @ref mpipe_ipc_src, so that the two halves compose as one
 * pipeline rather than as two programs that happen to share memory.
 *
 * From the libMP IPC plugin (Zephyr PR #114088), adapted to the mpipe element
 * model. Buffers cross by reference: the sink holds a reference and sends the
 * address, the source wraps that address in a buffer of its own and pushes it
 * downstream, and the reference is dropped only when the far side has finished
 * with it. The samples are never copied and never travel in a message.
 *
 * The pool the sink draws from must therefore live in memory the peer can
 * address. Nothing here can check that -- a local pointer is a valid pointer
 * either way -- so it is a property of how the pipeline is built.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PLUGIN_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PLUGIN_H_

/**
 * @defgroup mpipe_ipc_plugin IPC plugin
 * @ingroup mpipe_ipc
 * @brief Sink and source elements that carry a pipeline between two cores.
 * @{
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_msg.h>
#include <zephyr/mpipe/mpipe_sink.h>
#include <zephyr/mpipe/mpipe_src.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Sink that forwards buffers to a peer core. */
struct mpipe_ipc_sink {
	/** Base sink element. */
	struct mpipe_sink base;
	/** Endpoint this sink sends on. */
	struct ipc_ept ept;
	/**
	 * Kept because IPC Service stores a pointer to it, not a copy: the
	 * backend calls back through `cfg.cb` long after registration returns,
	 * so a caller's stack copy becomes a jump to whatever replaced it.
	 */
	struct ipc_ept_cfg cfg;
	/** True once the peer's source has bound to that endpoint. */
	bool bound;
	/**
	 * Buffers handed to the peer and not yet released.
	 *
	 * A slot is the unit of flow control: with none free the sink has as
	 * much outstanding as the peer can be trusted with, and refuses rather
	 * than overwrite memory the peer may still be reading.
	 */
	struct net_buf *pending[CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS];
	/** Buffers dropped because the peer had not released any slot. */
	uint32_t dropped;
};

/** @brief Source that receives buffers from a peer core. */
struct mpipe_ipc_src {
	/** Base source element. */
	struct mpipe_src base;
	/** Endpoint this source receives on. */
	struct ipc_ept ept;
	/** Kept for the same reason as the sink's: the backend holds a pointer. */
	struct ipc_ept_cfg cfg;
	/** True once bound to the peer's sink. */
	bool bound;
	/** True once the rest of the pipeline exists and can be pushed to. */
	bool running;
	/** Buffers received that could not be wrapped and were returned. */
	uint32_t refused;
};

/**
 * @brief Initialise an IPC sink on an open IPC Service instance.
 *
 * @param sink     Element to initialise.
 * @param id       Element identifier within the pipeline.
 * @param instance IPC Service instance, already opened.
 * @param name     Endpoint name; the peer source must use the same one.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument.
 * @retval other from ipc_service_register_endpoint().
 */
int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct device *instance, const char *name);

/**
 * @brief Initialise an IPC source on an open IPC Service instance.
 *
 * @param src      Element to initialise.
 * @param id       Element identifier within the pipeline.
 * @param instance IPC Service instance, already opened.
 * @param name     Endpoint name; must match the peer sink's.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument.
 * @retval other from ipc_service_register_endpoint().
 */
int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct device *instance, const char *name);

/**
 * @brief Set the format the source presents downstream.
 *
 * The plugin does not negotiate a format across the link yet, so both halves
 * are told the same one. Getting this wrong is silent: every element agrees and
 * the audio is simply wrong.
 */
int mpipe_ipc_src_set_format(struct mpipe_ipc_src *src,
			     const struct mpipe_structure *caps);

/** @brief True once both halves have bound to each other. */
bool mpipe_ipc_sink_is_bound(const struct mpipe_ipc_sink *sink);

/**
 * @brief Let the source begin delivering downstream.
 *
 * Call once the pipeline is built and playing. Registering the endpoint is what
 * makes the peer start sending, and that necessarily happens before the rest of
 * the pipeline exists; until this is called, arriving buffers are handed
 * straight back instead of pushed into a pad that is not linked yet.
 */
int mpipe_ipc_src_start(struct mpipe_ipc_src *src);

/** @brief True once both halves have bound to each other. */
bool mpipe_ipc_src_is_bound(const struct mpipe_ipc_src *src);

/** @brief Translate a local pointer to an address the peer can use. */
uintptr_t mpipe_ipc_virt_to_phys(void *virt_addr);

/** @brief Translate a peer address back into a local pointer. */
void *mpipe_ipc_phys_to_virt(uintptr_t phys_addr);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PLUGIN_H_ */
