/*
 * Copyright 2026 NXP
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

/** @brief Mask of a bring-up state inside a published state word. */
#define MPIPE_IPC_STATE_MASK 0xffffU

/**
 * @brief Pack a bring-up state together with the session that published it.
 *
 * A core publishes two words and a reader cannot sample both at once, so a
 * state read on its own says nothing about which incarnation meant it. The
 * mixture that matters is a leftover READY paired with a restarted peer's new
 * session word: taken together they read as an invitation into rings the new
 * incarnation has not built yet. Stamping the state with its own session makes
 * the word self-describing, so the pair is checked rather than assumed -- and
 * costs nothing at runtime, unlike a separate sequence counter, which would add
 * a word to the block and two stores to every publication to say the same
 * thing. It also means a diagnostic reader outside this code -- Linux, a
 * debugger -- can tell a live state from a dead one without decoding the
 * handshake.
 */
#define MPIPE_IPC_STATE_WORD(sid, state)                                               \
	(((uint32_t)(state) & MPIPE_IPC_STATE_MASK) |                                  \
	 (((uint32_t)(sid) & MPIPE_IPC_SID_MASK) << MPIPE_IPC_SID_BITS))

/** @brief The bring-up state held in a published state word. */
#define MPIPE_IPC_STATE_OF(word) ((uint32_t)((word) & MPIPE_IPC_STATE_MASK))
/** @brief The session that published a state word. */
#define MPIPE_IPC_STATE_SID(word)                                                      \
	((uint16_t)(((word) >> MPIPE_IPC_SID_BITS) & MPIPE_IPC_SID_MASK))

/**
 * @brief Polls a host waits on an unmoving READY peer before calling it residue.
 *
 * A core killed by `remoteproc stop` while it was READY leaves that word behind
 * in retained memory, and nothing in shared memory distinguishes it from a peer
 * that is still running. A live peer answers a host's new session within one of
 * its own poll periods, so a peer whose published word has not moved for
 * appreciably longer than that is gone. The sample polls a running link every
 * 500 ms and bring-up every 20 ms, so this is four running periods of margin.
 *
 * A core that is hung with its memory intact is indistinguishable from one that
 * is gone. Only the lifecycle authority knows the difference, and it is not in
 * this path; the timeout is the recovery.
 */
#define MPIPE_IPC_RESIDUE_POLLS 100U

/** @brief One endpoint's view of the session with its peer. */
struct mpipe_ipc_session {
	/** This core's session for the current boot. Never MPIPE_IPC_SID_NONE. */
	uint16_t local_sid;
	/**
	 * The peer request half last observed, echoed as this core's
	 * acknowledgement.
	 *
	 * Deliberately separate from @ref remote_sid. This is what we have
	 * *seen*; @ref remote_sid is what we have *accepted*. A core must echo
	 * unconditionally -- that echo is the only way the peer learns this core
	 * exists -- but must accept only a peer that has echoed back this boot's
	 * own session.
	 */
	uint16_t peer_seen;
	/** The peer session latched at connect, or MPIPE_IPC_SID_NONE. */
	uint16_t remote_sid;
	/** True once a peer session has been latched and not since invalidated. */
	bool connected;
	/** Consecutive polls in which the peer published nothing new. */
	uint16_t peer_stall;
	/** The peer word and state behind @ref peer_stall. */
	uint32_t peer_last_word;
	uint32_t peer_last_state;
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

/**
 * @brief Record what the peer is advertising and report whether it is live.
 *
 * Always updates the acknowledgement this core will publish, so the peer can
 * see it. Returns true only when the peer has acknowledged *this boot's*
 * session, which is the one thing residue from a dead core cannot fake.
 */
bool mpipe_ipc_session_observe(struct mpipe_ipc_session *session, uint32_t peer_word);

/**
 * @brief Bring-up state each core publishes beside its session word.
 *
 * This exists because OpenAMP's host zeroes **both** shared vrings on every
 * `ipc_service_open_instance()` (`virtio_create_virtqueues`), the remote writes
 * nothing to shared memory at init, and `virtio_reset_device()` has no callers
 * anywhere in OpenAMP or Zephyr. There is therefore no way to reconcile a ring
 * whose indices moved underneath a live peer: whoever restarts must be kept
 * away from the rings until the other side has stood down.
 */
enum mpipe_ipc_bringup_state {
	/** Not initialised, or safely torn down. The rings are no longer owned. */
	MPIPE_IPC_BRINGUP_DOWN = 0,
	/** Session published; standing by, not touching the rings. */
	MPIPE_IPC_BRINGUP_CLAIMED = 1,
	/** Rings initialised and in use. */
	MPIPE_IPC_BRINGUP_READY = 2,
	/**
	 * Fault detected; the rings may still be owned and must not be opened.
	 * Recovery requires the lifecycle authority.
	 */
	MPIPE_IPC_BRINGUP_FAULT = 3,
};

/** @brief What a core should do next, given what its peer is publishing. */
enum mpipe_ipc_bringup_action {
	/** Keep polling; it is not safe to proceed yet. */
	MPIPE_IPC_ACTION_WAIT = 0,
	/** Safe to call ipc_service_open_instance() now. */
	MPIPE_IPC_ACTION_OPEN = 1,
	/** The peer restarted, or is publishing something impossible. Tear down. */
	MPIPE_IPC_ACTION_FAULT = 2,
};

/**
 * @brief Decide the next bring-up step from the peer's published state.
 *
 * The two roles have opposite preconditions, which is what makes the barrier
 * deadlock-free in both restart directions:
 *
 * - The **host** owns the rings and clears them, so it may open only while the
 *   peer is `DOWN` or `CLAIMED`, never `READY` or `FAULT`. That is what stops a
 *   restarted host wiping a live remote's receive ring.
 * - The **remote** writes nothing at init and reads rings the host built, so it
 *   may open only once the peer *is* `READY`.
 *
 * A peer is accepted only once it acknowledges **this boot's** session. That
 * rule is what makes retained memory safe to read: a word left behind by a core
 * that is no longer running carries a stale acknowledgement, and
 * mpipe_ipc_session_next() guarantees this boot's session differs from the one
 * the peer last acknowledged, so residue can never satisfy the test. Latching on
 * the mere presence of a session instead would make a dead peer's leftover word
 * indistinguishable from a live peer -- observed on the board, where the host
 * latched a session from the remote's previous life and then read the remote's
 * actual boot as a restart.
 *
 * The acknowledgement is echoed unconditionally, including to residue, because
 * otherwise neither side would ever ack first and both would wait forever.
 * Echoing is safe: mpipe_ipc_session_next() skips whatever the peer last
 * acknowledged, so a peer booting afterwards cannot pick the echoed value.
 *
 * A host does not require the acknowledgement before *waiting*, only before
 * proceeding: it holds off on any peer that looks READY, and falls through after
 * @ref MPIPE_IPC_RESIDUE_POLLS unmoving polls. Requiring the acknowledgement to
 * wait would let a restarted host barge in during the window before a live
 * remote has noticed it, which is the ring wipe this barrier exists to prevent.
 *
 * @p session is updated in place: acceptance latches the peer, and a restart
 * clears the latch, so a caller that faults and re-enters the sequence starts
 * from a clean state.
 *
 * @p require_peer makes the host wait for a live peer instead of opening alone.
 * Set it when the mailbox is powered by the peer's domain rather than this
 * core's: on the i.MX8MP, MU3 sits in AUDIOMIX and its clock is a peripheral
 * clock of the DSP node, so it runs only while Linux holds the DSP
 * runtime-resumed. A host that opens first then configures a mailbox that is
 * not clocked, and the writes are silently discarded -- the link comes up, both
 * cores agree, and not one doorbell is ever delivered to the host. Waiting costs
 * nothing, because a host has no peer to talk to until the peer exists anyway.
 *
 * @param session           This core's session, from mpipe_ipc_session_open().
 * @param is_host           True on the static-vrings host.
 * @param require_peer      Host must not open until the peer acknowledges it.
 * @param peer_session_word The peer's published handshake word.
 * @param peer_state_word   The peer's published state word, as written by
 *                          @ref MPIPE_IPC_STATE_WORD. FAULT and unknown states
 *                          produce @ref MPIPE_IPC_ACTION_FAULT. A state stamped
 *                          with a session other than the one @p
 *                          peer_session_word requests is not this incarnation's
 *                          and is never acted on: the two words were sampled
 *                          across a peer restart, so the caller waits and reads
 *                          again.
 */
enum mpipe_ipc_bringup_action mpipe_ipc_bringup_step(struct mpipe_ipc_session *session,
						     bool is_host, bool require_peer,
						     uint32_t peer_session_word,
						     uint32_t peer_state_word);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_SESSION_H_ */
