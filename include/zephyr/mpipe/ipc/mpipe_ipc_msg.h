/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Wire messages between an IPC sink and its peer IPC source.
 *
 * From the libMP IPC plugin (Zephyr PR #114088), adapted to the mpipe element
 * model. This is the element plugin's own protocol and is distinct from the
 * link's control protocol in mpipe_ipc_protocol.h: that one brings the link up,
 * this one carries a running pipeline across it.
 *
 * Audio does not travel in these messages. A buffer is handed over by
 * reference -- region offset, size and an identifier -- and the consumer reads
 * the samples in place out of shared memory, then returns the identifier so the
 * producer can free the buffer. The identifier, rather than the address, is
 * what comes back: it is what makes a release cheap to validate.
 *
 * @ref mpipe_ipc_msg is the decoded form, used by the code on either side. It
 * is never sent as itself. What travels is the byte layout that
 * mpipe_ipc_msg_encode() writes: fixed-width, little-endian, with a version and
 * no padding a compiler chose. Two cores agreeing on a C struct would mean
 * agreeing on their compilers' ABIs and on CONFIG_MPIPE_STRUCTURE_MAX_FIELDS,
 * none of which either side can check -- and the padding inside such a struct
 * would put whatever the stack last held on the wire.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Protocol version carried by every plugin message.
 *
 * A peer built against a different version is rejected message by message
 * rather than misread: the layouts below are only meaningful together with it.
 */
#define MPIPE_IPC_PLUGIN_VERSION 3U

/**
 * @brief Largest buffer identifier the protocol can carry.
 *
 * A ceiling on the wire format, not a policy: how many a sink actually keeps
 * outstanding is CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS, which is bounded by the
 * pool it draws from.
 */
#define MPIPE_IPC_MAX_BUFFERS 64

/**
 * @brief Most capability fields one CAPS message can carry.
 *
 * A structure holds at most one value per field identifier, so there is nothing
 * to carry beyond the number of identifiers that exist.
 */
#define MPIPE_IPC_WIRE_MAX_CAPS_FIELDS MPIPE_CAPS_END

/** @brief Bytes every message begins with: version, type, and padding. */
#define MPIPE_IPC_WIRE_HEADER_LEN 4U

/** @brief Bytes one capability field occupies inside a CAPS message. */
#define MPIPE_IPC_WIRE_FIELD_LEN 16U

/** @brief Longest message the protocol can produce, which is a full CAPS. */
#define MPIPE_IPC_WIRE_MAX_LEN                                                                 \
	(MPIPE_IPC_WIRE_HEADER_LEN + 4U +                                                      \
	 (MPIPE_IPC_WIRE_MAX_CAPS_FIELDS * MPIPE_IPC_WIRE_FIELD_LEN))

/** @brief Message types exchanged between an IPC sink and source. */
enum mpipe_ipc_msg_type {
	/** A buffer in the shared region is handed to the peer. */
	MPIPE_IPC_MSG_DATA_BUFFER = 0,
	/** The peer is done with a buffer it was handed. */
	MPIPE_IPC_MSG_DATA_RELEASE,
	/** A pipeline event, forwarded across the link. */
	MPIPE_IPC_MSG_EVENT,
	/** The format the sending half settled on. */
	MPIPE_IPC_MSG_CAPS,
	/** One past the last type. */
	MPIPE_IPC_MSG_END,
};

/** @brief One message, decoded. Never sent as itself; see the file comment. */
struct mpipe_ipc_msg {
	/** One of @ref mpipe_ipc_msg_type. */
	uint8_t type;
	union {
		/** A buffer handed over by reference. */
		struct {
			/** Byte offset within the negotiated shared region. */
			uint32_t offset;
			/** Bytes of payload at @ref offset. */
			uint32_t size;
			/** Capture timestamp, in the pipeline's own units. */
			uint32_t timestamp;
			/** Which of the sender's slots this buffer occupies. */
			uint16_t buffer_id;
			/** Session of the core that owns the referenced storage. */
			uint16_t generation;
		} data;

		/** The consumer is done with a buffer. */
		struct {
			/** The slot being returned. */
			uint16_t buffer_id;
			/** Ownership generation copied from the matching DATA. */
			uint16_t generation;
		} release;

		/** A pipeline event. */
		struct {
			/** One of @ref mpipe_dispatch_type. */
			uint8_t event_type;
		} event;

		/**
		 * The format the sender's half of the pipeline settled on.
		 *
		 * Sent rather than configured separately on both cores. A
		 * format the two halves are each told is a format they can
		 * silently disagree about: every element accepts, and the audio
		 * is simply wrong.
		 */
		struct mpipe_structure caps;
	};
};

/**
 * @brief Serialize a message into its wire form.
 *
 * @param dst      Where to write; at least @ref MPIPE_IPC_WIRE_MAX_LEN bytes is
 *                 always enough.
 * @param capacity Bytes available at @p dst.
 * @param msg      Message to encode.
 * @param written  Set to the number of bytes written on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument, an unknown type, or a value the wire
 *         format cannot represent.
 * @retval -ENOSPC if @p capacity is too small for this message.
 */
int mpipe_ipc_msg_encode(void *dst, size_t capacity, const struct mpipe_ipc_msg *msg,
			 size_t *written);

/**
 * @brief Parse a received message, rejecting anything malformed.
 *
 * Nothing is written to @p msg unless the whole message passes: the version,
 * the exact length for the type, the reserved bytes, and for CAPS every
 * identifier, type and range in the structure. A caller therefore never has to
 * re-check what it decoded, only what depends on its own state -- whether a
 * buffer identifier is one it handed out, whether an offset lies in its region.
 *
 * @param msg    Filled in on success.
 * @param src    Received bytes.
 * @param length Number of bytes received.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a NULL argument.
 * @retval -EMSGSIZE if @p length does not match the message's type.
 * @retval -ENOTSUP if the version or the type is not one this build speaks.
 * @retval -EPROTO if a field is out of range or a reserved byte is nonzero.
 */
int mpipe_ipc_msg_decode(struct mpipe_ipc_msg *msg, const void *src, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_ */
