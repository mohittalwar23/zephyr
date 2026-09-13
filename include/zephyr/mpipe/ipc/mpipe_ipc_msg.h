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
 * reference -- address, size and an identifier -- and the consumer reads the
 * samples in place out of shared memory, then returns the identifier so the
 * producer can free the buffer. The identifier, rather than the address, is
 * what comes back: it is what makes a release cheap to validate.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_
#define ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Largest serialized payload carried inside a message. */
#define MPIPE_IPC_MAX_SERIALIZED_PAYLOAD 64

/**
 * @brief Largest buffer identifier the protocol can carry.
 *
 * A ceiling on the wire format, not a policy: how many a sink actually keeps
 * outstanding is CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS, which is bounded by the
 * pool it draws from.
 */
#define MPIPE_IPC_MAX_BUFFERS 64

/** @brief Message types exchanged between an IPC sink and source. */
enum mpipe_ipc_msg_type {
	MPIPE_IPC_MSG_STATE_CHANGE = 0,
	MPIPE_IPC_MSG_PROPERTY_SET,
	MPIPE_IPC_MSG_DATA_BUFFER,
	MPIPE_IPC_MSG_DATA_RELEASE,
	MPIPE_IPC_MSG_EVENT,
	MPIPE_IPC_MSG_CAPS,
	MPIPE_IPC_MSG_QUERY_REQ,
	MPIPE_IPC_MSG_QUERY_RESP,
	MPIPE_IPC_MSG_BUS,
};

BUILD_ASSERT(sizeof(struct mpipe_structure) <= 128,
	     "a caps structure must stay small enough to carry in one message");

/** @brief One message. Fixed size, so a short read is a protocol error. */
struct mpipe_ipc_msg {
	uint32_t type;
	union {
		struct {
			uint32_t state;
		} state_cmd;

		struct {
			uint32_t element_id;
			uint32_t prop_id;
			uint32_t prop_val;
		} prop_cmd;

		/** A buffer handed over by reference. */
		struct {
			uint32_t phys_addr;
			uint32_t size;
			uint32_t timestamp;
			uint32_t buffer_id;
		} data;

		/** The consumer is done with a buffer. */
		struct {
			uint32_t buffer_id;
		} release;

		struct {
			uint8_t event_type;
			uint8_t payload[MPIPE_IPC_MAX_SERIALIZED_PAYLOAD];
		} event;

		/**
		 * The format the sink's half of the pipeline settled on.
		 *
		 * Sent rather than configured separately on both cores. A
		 * format the two halves are each told is a format they can
		 * silently disagree about: every element accepts, and the audio
		 * is simply wrong.
		 *
		 * A structure is flat and holds no pointers, so it travels as
		 * itself.
		 */
		struct mpipe_structure caps;

		struct {
			uint8_t query_type;
			bool success;
			uint8_t payload[MPIPE_IPC_MAX_SERIALIZED_PAYLOAD];
		} query;

		struct {
			uint32_t msg_type;
			uint8_t payload[MPIPE_IPC_MAX_SERIALIZED_PAYLOAD];
		} bus_msg;
	};
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MPIPE_IPC_MPIPE_IPC_MSG_H_ */
