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
 * model. Buffers cross by reference: the sink holds a reference and sends an
 * offset within an explicitly configured shared region, the source validates
 * and wraps that offset, and the reference is dropped only when the far side
 * has finished with it. The samples are never copied and never travel in a
 * message.
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
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_msg.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>
#include <zephyr/mpipe/mpipe_sink.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_src.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Memory window whose contents may be shared with the peer. */
struct mpipe_ipc_region {
	/** Local mapping of byte offset zero. */
	void *base;
	/** Number of bytes accessible from @ref base. */
	size_t size;
	/** Required alignment of buffer offsets and lengths; must be a power of two. */
	size_t align;
};

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
	/** Transport session that authorizes traffic on this endpoint. */
	struct mpipe_ipc_transport *transport;
	/** Only memory in this region may be handed to the peer. */
	struct mpipe_ipc_region region;
	/** True once the peer's source has bound to that endpoint. */
	atomic_t bound;
	/** Atomic closed flag plus admitted-operation count. */
	atomic_t operation_state;
	/** Wakes teardown when the last admitted operation leaves. */
	struct k_sem operations_drained;
	/** Nonzero while the endpoint is registered. */
	atomic_t registered;
	/** Protects pending slots and their generations. */
	struct k_spinlock pending_lock;
	/**
	 * Buffers handed to the peer and not yet released.
	 *
	 * A slot is the unit of flow control: with none free the sink has as
	 * much outstanding as the peer can be trusted with, and refuses rather
	 * than overwrite memory the peer may still be reading.
	 */
	struct net_buf *pending[CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS];
	/** Storage-owner session associated with each pending slot. */
	uint16_t pending_generation[CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS];
	/** Buffers dropped because the peer had not released any slot. */
	atomic_t dropped;
	/** The format this half settled on, sent to the peer. */
	struct mpipe_structure caps;
	/** Nonzero once a format has been settled. */
	atomic_t have_caps;
	/** Protects the format copied between pipeline and bound callback contexts. */
	struct k_spinlock caps_lock;
	/** The base sink's event handler, still run after forwarding. */
	int (*base_event_fn)(struct mpipe_pad *pad, struct mpipe_dispatch *event);
};

/** @brief Source that receives buffers from a peer core. */
struct mpipe_ipc_src {
	/** Base source element. */
	struct mpipe_src base;
	/** Endpoint this source receives on. */
	struct ipc_ept ept;
	/** Kept for the same reason as the sink's: the backend holds a pointer. */
	struct ipc_ept_cfg cfg;
	/** Transport session that authorizes traffic on this endpoint. */
	struct mpipe_ipc_transport *transport;
	/** Local mapping of the peer-visible payload window. */
	struct mpipe_ipc_region region;
	/** True once bound to the peer's sink. */
	atomic_t bound;
	/** Atomic closed flag plus admitted-operation count. */
	atomic_t operation_state;
	/** Wakes teardown when the last admitted callback leaves. */
	struct k_sem operations_drained;
	/** Nonzero while the endpoint is registered. */
	atomic_t registered;
	/** Per-instance wrapper pool identity used by wrapper destructors. */
	struct mpipe_buffer_pool wrapper_pool;
	/** Buffer identifiers currently represented by a live wrapper. */
	ATOMIC_DEFINE(live, CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS);
	/** Buffers received that could not be wrapped and were returned. */
	atomic_t refused;
	/** The format the peer announced. */
	struct mpipe_structure caps;
	/** Nonzero once the peer has announced one. */
	atomic_t have_caps;
	/** Buffers whose release could not be sent, one bit each, retried later. */
	ATOMIC_DEFINE(unreleased, CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS);
	/** Ownership generation associated with each deferred release. */
	uint16_t unreleased_generation[CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS];
	/** Protects deferred-release generations and their bitmap. */
	struct k_spinlock release_lock;
	/** Retries releases even when the peer sends no further messages. */
	struct k_work_delayable release_work;
	/** Storage used while synchronously cancelling release work. */
	struct k_work_sync release_sync;
	/** How many releases have had to be deferred. */
	atomic_t deferred;
};

/**
 * @brief Initialise an IPC sink on an open IPC Service instance.
 *
 * @param sink     Element to initialise.
 * @param id       Element identifier within the pipeline.
 * @param instance IPC Service instance, already opened.
 * @param name     Endpoint name; the peer source must use the same one.
 * @param region   Local mapping of the shared payload window.
 * @param transport Live transport that owns @p instance.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument or if the transport does not own the instance.
 * @retval -ENOTCONN if the transport is not live.
 * @retval -ECONNRESET if the peer session changed before registration.
 * @retval other from ipc_service_register_endpoint().
 */
int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct device *instance, const char *name,
			const struct mpipe_ipc_region *region,
			struct mpipe_ipc_transport *transport);

/**
 * @brief Close admission, drain callbacks, and deregister a sink endpoint.
 *
 * Outstanding DATA references are deliberately retained: only a matching
 * generation-scoped RELEASE or the system's paired reset makes them reusable.
 * The sink object must therefore remain alive until reset when slots are still
 * outstanding.
 *
 * Safe to call repeatedly.
 */
int mpipe_ipc_sink_deinit(struct mpipe_ipc_sink *sink);

/**
 * @brief Initialise an IPC source on an open IPC Service instance.
 *
 * @param src      Element to initialise.
 * @param id       Element identifier within the pipeline.
 * @param instance IPC Service instance, already opened.
 * @param name     Endpoint name; must match the peer sink's.
 * @param region   Local mapping of the shared payload window.
 * @param transport Live transport that owns @p instance.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument or if the transport does not own the instance.
 * @retval -ENOTCONN if the transport is not live.
 * @retval -ECONNRESET if the peer session changed before registration.
 * @retval other from ipc_service_register_endpoint().
 */
int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct device *instance, const char *name,
		       const struct mpipe_ipc_region *region,
		       struct mpipe_ipc_transport *transport);

/**
 * @brief Close admission, cancel retries, and deregister a source endpoint.
 *
 * The pipeline must be stopped first so no downstream owner can retain a
 * wrapper beyond the source object's lifetime. Safe to call repeatedly.
 */
int mpipe_ipc_src_deinit(struct mpipe_ipc_src *src);

/**
 * @brief Set the format the source presents downstream.
 *
 * Rarely needed: the peer's sink announces the format it settled on, and the
 * source applies that. Use this only to seed a format before the peer has
 * attached -- a format configured independently on both cores is one the two
 * can silently disagree about.
 */
int mpipe_ipc_src_set_format(struct mpipe_ipc_src *src,
			     const struct mpipe_structure *caps);

/** @brief True once the peer has announced the format it settled on. */
bool mpipe_ipc_src_has_caps(const struct mpipe_ipc_src *src);

/** @brief True once both halves have bound to each other. */
bool mpipe_ipc_sink_is_bound(const struct mpipe_ipc_sink *sink);

/** @brief True once both halves have bound to each other. */
bool mpipe_ipc_src_is_bound(const struct mpipe_ipc_src *src);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_PLUGIN_H_ */
