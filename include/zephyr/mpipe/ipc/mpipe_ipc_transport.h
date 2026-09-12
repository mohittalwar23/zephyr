/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bring-up owner for the direct M7 to HiFi4 link.
 *
 * This is the piece that turns the session handshake and the rendezvous
 * barrier into a working instance. It owns the shared control block, decides
 * when it is safe to open the IPC Service instance, and publishes this core's
 * readiness for the peer to observe.
 *
 * It exists as a separate layer because opening the instance is the dangerous
 * operation: the static-vrings host clears **both** shared vrings inside
 * `ipc_service_open_instance()`, and OpenAMP has no path to reconcile a ring
 * whose indices moved underneath a live reader. So the decision of *when* to
 * open has to be made from shared state before the call, not recovered from
 * afterwards.
 *
 * Bring-up never depends on a doorbell. Readiness is established by polling
 * state the peer has published, which sidesteps the whole class of lost or
 * coalesced startup notifications that upstream backends handle
 * inconsistently across silicon. It also supplies the bounded wait that
 * static-vrings lacks: OpenAMP's remote spins forever inside
 * `rpmsg_virtio_wait_remote_ready()`, and that spin is simply never entered,
 * because this layer does not call open until the peer is ready.
 *
 * The backend is injected as @ref mpipe_ipc_ops so the sequencing can be
 * driven and asserted on a host, where both restart orderings are cheap to
 * reproduce and a real two-core race is not.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_TRANSPORT_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_TRANSPORT_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_session.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bytes reserved for one core's published state.
 *
 * The window is uncached on both cores — by MPU attribute on the M7 and by the
 * reset CACHEATTR bypass region on the HiFi4 — so this padding is not required
 * for correctness. It is kept so the two cores never share a line if the window
 * is ever made cacheable, and sized for the larger of the two line sizes.
 */
#define MPIPE_IPC_CORE_BLOCK_SIZE 128U

/** @brief One core's published bring-up state. */
struct mpipe_ipc_core_block {
	/** Session handshake word: request low, acknowledgement high. */
	uint32_t session;
	/** One of @ref mpipe_ipc_bringup_state. */
	uint32_t state;
};

/**
 * @brief The shared control block, at a fixed address both cores agree on.
 *
 * Each core writes only its own half and reads only the peer's, so no lock is
 * needed: every field is a single naturally aligned 32-bit word with exactly
 * one writer.
 */
struct mpipe_ipc_shared {
	struct mpipe_ipc_core_block host;
	uint8_t host_pad[MPIPE_IPC_CORE_BLOCK_SIZE -
			 sizeof(struct mpipe_ipc_core_block)];
	struct mpipe_ipc_core_block remote;
	uint8_t remote_pad[MPIPE_IPC_CORE_BLOCK_SIZE -
			   sizeof(struct mpipe_ipc_core_block)];
};

BUILD_ASSERT(sizeof(struct mpipe_ipc_shared) == 2U * MPIPE_IPC_CORE_BLOCK_SIZE,
	     "the control block must be exactly two padded core blocks");

/**
 * @brief Injected backend, so bring-up can be driven on a host.
 *
 * @c open_instance is the operation the barrier exists to gate; on the real
 * backend it is `ipc_service_open_instance()`.
 */
struct mpipe_ipc_ops {
	/** Open the physical instance. The host clears both vrings here. */
	int (*open_instance)(void *context);
	/** Close the physical instance, if the backend supports it. */
	int (*close_instance)(void *context);
	/** Read one 32-bit word of the shared control block. */
	uint32_t (*load)(const volatile uint32_t *address);
	/** Write one 32-bit word of the shared control block. */
	void (*store)(volatile uint32_t *address, uint32_t value);
};

/** @brief Bring-up outcome, distinguishing "not yet" from "never". */
enum mpipe_ipc_transport_state {
	/** Not started. */
	MPIPE_IPC_TRANSPORT_IDLE = 0,
	/** Session published; waiting on the peer. */
	MPIPE_IPC_TRANSPORT_WAITING = 1,
	/** Instance open and readiness published. */
	MPIPE_IPC_TRANSPORT_RUNNING = 2,
	/** The peer restarted or misbehaved; torn down. */
	MPIPE_IPC_TRANSPORT_FAULTED = 3,
};

/** @brief One core's view of the link. */
struct mpipe_ipc_transport {
	volatile struct mpipe_ipc_shared *shared;
	const struct mpipe_ipc_ops *ops;
	void *context;
	struct mpipe_ipc_session session;
	enum mpipe_ipc_transport_state state;
	bool is_host;
	/** Bring-up polls this core attempted before succeeding or giving up. */
	uint32_t poll_count;
};

/**
 * @brief Bind a transport to its shared block and backend, and claim a session.
 *
 * Reads this core's previously published word back out of shared memory and
 * increments past it, which is what makes a restart detectable; a session
 * derived from local state would repeat on every boot. Publishes the new
 * session with state @c CLAIMED, so a live peer can see that this core
 * restarted before anything touches a ring.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument or an incomplete @p ops.
 */
int mpipe_ipc_transport_init(struct mpipe_ipc_transport *transport,
			     volatile struct mpipe_ipc_shared *shared,
			     const struct mpipe_ipc_ops *ops, void *context,
			     bool is_host);

/**
 * @brief Advance bring-up by one poll.
 *
 * Call until it stops returning @c -EAGAIN. On the step that is allowed to
 * proceed it opens the instance and publishes @c READY.
 *
 * @retval 0           the instance is open and this core is READY.
 * @retval -EAGAIN     not yet safe; call again.
 * @retval -ECONNRESET the peer restarted or published something impossible.
 *                     The transport is faulted and must be re-initialised.
 * @retval -EINVAL     @p transport is NULL or was never initialised.
 */
int mpipe_ipc_transport_poll(struct mpipe_ipc_transport *transport);

/**
 * @brief Check a peer word before acting on a received message.
 *
 * Thin wrapper over mpipe_ipc_session_check() that also faults the transport,
 * so a caller cannot consume a message from an incarnation it never handshook
 * with.
 *
 * @retval 0           safe to deliver.
 * @retval -ECONNRESET the peer restarted; the transport is faulted.
 * @retval -ENOTCONN   no peer session has been latched.
 * @retval -EINVAL     @p transport is NULL.
 */
int mpipe_ipc_transport_check_peer(struct mpipe_ipc_transport *transport);

/**
 * @brief Stand down: publish DOWN so a restarted peer may proceed.
 *
 * This is the other half of the barrier. A survivor that has detected a peer
 * restart must reach this state before that peer can safely open, because the
 * peer will clear rings this core would otherwise still be reading.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p transport is NULL.
 */
int mpipe_ipc_transport_quiesce(struct mpipe_ipc_transport *transport);

/**
 * @brief Default backend, wiring @ref mpipe_ipc_ops to Zephyr's IPC Service.
 *
 * The context passed to mpipe_ipc_transport_init() must be the
 * `const struct device *` of the IPC instance. Shared-block accesses are plain
 * volatile 32-bit loads and stores with no cache maintenance, which is correct
 * here because the window is uncached on both cores: by MPU attribute on the
 * Cortex-M7, and by the reset CACHEATTR bypass region on the HiFi4.
 *
 * Available only when `CONFIG_IPC_SERVICE` is enabled; the transport logic
 * itself has no such dependency, which is what lets it be tested on a host.
 */
extern const struct mpipe_ipc_ops mpipe_ipc_zephyr_ops;

/** @brief This core's published word, for diagnostics. */
uint32_t mpipe_ipc_transport_local_word(const struct mpipe_ipc_transport *transport);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_TRANSPORT_H_ */
