/*
 * Copyright (c) 2026 Mohit Talwar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Session handshake for the direct M7 to HiFi4 link.
 *
 * Linux is the lifecycle authority for both cores but is not in the data path,
 * so either core can be restarted underneath the other. The survivor must
 * notice and refuse to act on state from its peer's previous life.
 *
 * The design is Zephyr ICMsg's, at `subsys/ipc/ipc_service/lib/icmsg.c`. It is
 * the only mechanism found in a survey of TI, Qualcomm, AMD/Xilinx, ST, Analog
 * Devices, Espressif and the virtio specification that is both a counter and
 * detection-based: TI has no epoch at any layer and requires both ends to
 * restart together, and Qualcomm relies on an out-of-band notification from a
 * lifecycle authority that talks to survivors, which ours does not. The
 * algorithm is reimplemented here rather than reused because `CONFIG_PBUF` is
 * declared inside `if IPC_SERVICE_ICMSG`, and ICMsg itself is copy-only and
 * single-endpoint so it cannot carry zero-copy audio.
 *
 * Each direction owns one 32-bit word in shared memory: the low half is the
 * writer's own session request, the high half acknowledges the session it last
 * saw from its peer. Carrying the acknowledgement matters — a one-way stamp
 * tells the survivor that its peer changed but gives the peer no way to learn
 * that the survivor noticed.
 *
 * Two properties are load-bearing and were verified rather than assumed:
 *
 * - A new session is derived by reading the previous value back out of shared
 *   memory and incrementing it, never from local state. A core that boots with
 *   session 1 every time is indistinguishable from one that never restarted.
 * - That read-back is only legal because the region survives a restart. Tested
 *   on the board 2026-09-12: a reserved region outside both remoteproc
 *   carveout lists is byte-perfectly retained across an M7 restart, a DSP
 *   restart, repeated cycles, and both cores running together.
 *
 * The word must live in its own cache-line-padded reserved region. It must not
 * be placed in the spare bytes of the static-vrings status area, because
 * `CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET` memsets that whole region at
 * `PRE_KERNEL_1` and would erase the value being retained.
 *
 * Detection alone is not a complete recovery story. OpenAMP's host zeroes both
 * entire vrings on every open, so a restarted host wipes a live peer's receive
 * ring before any message can be exchanged. A rendezvous barrier above this
 * layer must stop the survivor touching a ring until the peer has published a
 * new session and finished initialising.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SESSION_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SESSION_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reserved session value meaning "no session established". */
#define MPIPE_IPC_SID_NONE 0U

/** @brief Width of one session identifier, in bits. */
#define MPIPE_IPC_SID_BITS 16U
/** @brief Mask of one session identifier. */
#define MPIPE_IPC_SID_MASK 0xffffU

/** @brief Pack a request and an acknowledgement into one shared word. */
#define MPIPE_IPC_HANDSHAKE(req, ack)                                                  \
	(((uint32_t)(req) & MPIPE_IPC_SID_MASK) |                                      \
	 (((uint32_t)(ack) & MPIPE_IPC_SID_MASK) << MPIPE_IPC_SID_BITS))

/** @brief The writer's own session request, from a shared word. */
#define MPIPE_IPC_HANDSHAKE_REQ(word) ((uint16_t)((word) & MPIPE_IPC_SID_MASK))
/** @brief The peer session the writer last acknowledged, from a shared word. */
#define MPIPE_IPC_HANDSHAKE_ACK(word)                                                  \
	((uint16_t)(((word) >> MPIPE_IPC_SID_BITS) & MPIPE_IPC_SID_MASK))

BUILD_ASSERT(MPIPE_IPC_SID_NONE == 0U, "zero is reserved for no session");

/** @brief One endpoint's view of the session with its peer. */
struct mpipe_ipc_session {
	/** This core's session for the current boot. Never MPIPE_IPC_SID_NONE. */
	uint16_t local_sid;
	/** The peer session latched at connect, or MPIPE_IPC_SID_NONE. */
	uint16_t remote_sid;
	/** True once a peer session has been latched and not since invalidated. */
	bool connected;
};

/**
 * @brief Choose this boot's session identifier.
 *
 * Increments past @p previous_req, skipping both the reserved zero and
 * @p peer_ack, so the new value cannot be mistaken for the session the peer
 * has already acknowledged.
 *
 * @param previous_req The request half of this core's own word, read back out
 *                     of shared memory before it is overwritten.
 * @param peer_ack     The acknowledgement half of the peer's word.
 *
 * @return A session identifier that is neither MPIPE_IPC_SID_NONE nor
 *         @p peer_ack.
 */
uint16_t mpipe_ipc_session_next(uint16_t previous_req, uint16_t peer_ack);

/**
 * @brief Begin a session, given the word this core previously published.
 *
 * Picks a fresh @c local_sid and clears any latched peer. The caller writes
 * the resulting word, obtained from mpipe_ipc_session_word(), to shared memory.
 *
 * @retval 0 always; @p session must not be NULL.
 * @retval -EINVAL if @p session is NULL.
 */
int mpipe_ipc_session_open(struct mpipe_ipc_session *session, uint32_t previous_word,
			   uint16_t peer_ack);

/** @brief The word this core should publish for its current state. */
uint32_t mpipe_ipc_session_word(const struct mpipe_ipc_session *session);

/**
 * @brief Latch the peer's session, completing the handshake.
 *
 * @retval 0 on success.
 * @retval -EINVAL   @p session is NULL.
 * @retval -EAGAIN   the peer has not published a session yet.
 */
int mpipe_ipc_session_latch(struct mpipe_ipc_session *session, uint32_t peer_word);

/**
 * @brief Check a peer word before acting on anything it delivered.
 *
 * Call this before delivering every received message, as ICMsg does. A change
 * in the peer's request half means the peer restarted; the session is marked
 * disconnected and the caller must fault rather than consume the message.
 *
 * @retval 0           the peer session is unchanged; the message may proceed.
 * @retval -EINVAL     @p session is NULL.
 * @retval -ENOTCONN   no peer session has been latched yet.
 * @retval -ECONNRESET the peer restarted. @p session is now disconnected.
 */
int mpipe_ipc_session_check(struct mpipe_ipc_session *session, uint32_t peer_word);

/** @brief True when the peer has acknowledged this core's session. */
bool mpipe_ipc_session_acknowledged(const struct mpipe_ipc_session *session,
				    uint32_t peer_word);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SESSION_H_ */
