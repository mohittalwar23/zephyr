/*
 * Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_plugin.h>
#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_dispatch.h>

LOG_MODULE_REGISTER(mpipe_ipc_plugin_sink, CONFIG_MPIPE_LOG_LEVEL);

static int sink_send(struct mpipe_ipc_sink *sink, const struct mpipe_ipc_msg *msg)
{
	int ret = ipc_service_send(&sink->ept, msg, sizeof(*msg));

	if (ret < 0) {
		return ret;
	}

	return ret == sizeof(*msg) ? 0 : -EMSGSIZE;
}

static void sink_received(const void *data, size_t len, void *priv)
{
	struct mpipe_ipc_sink *sink = priv;
	const struct mpipe_ipc_msg *msg = data;
	struct net_buf *buf;
	uint32_t id;

	/*
	 * Fixed-size messages, so a short read is a protocol error rather than
	 * something to parse defensively around.
	 */
	if (len != sizeof(*msg)) {
		LOG_ERR("message of %zu bytes, expected %zu", len, sizeof(*msg));
		return;
	}

	if (msg->type != MPIPE_IPC_MSG_DATA_RELEASE) {
		LOG_DBG("ignoring message type %u", msg->type);
		return;
	}

	id = msg->release.buffer_id;
	if (id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS || sink->pending[id] == NULL) {
		/*
		 * Either the peer invented an identifier or it released one
		 * twice. Neither is survivable by guessing, and dropping the
		 * reference anyway would free a buffer still in use.
		 */
		LOG_ERR("release for buffer %u, which is not outstanding", id);
		return;
	}

	buf = sink->pending[id];
	sink->pending[id] = NULL;
	net_buf_unref(buf);
}

static void sink_bound(void *priv)
{
	struct mpipe_ipc_sink *sink = priv;

	sink->bound = true;
	LOG_INF("peer source bound");

	/*
	 * The format was very likely settled before the peer attached, and it
	 * cannot ask for it. Send what we have.
	 */
	if (sink->have_caps) {
		struct mpipe_ipc_msg msg = {
			.type = MPIPE_IPC_MSG_CAPS,
			.caps = sink->caps,
		};

		(void)sink_send(sink, &msg);
	}
}

static int sink_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
			 struct net_buf **out_buf)
{
	struct mpipe_ipc_sink *sink;
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_msg msg;
	int id = -1;
	int ret;

	__ASSERT_NO_MSG(pad != NULL);
	__ASSERT_NO_MSG(in_buf != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	sink = CONTAINER_OF(pad->object.container, struct mpipe_ipc_sink,
			    base.element.object);
	meta = mpipe_buffer_get_meta(in_buf);

	/* A sink is the end of the chain on this core. */
	*out_buf = NULL;

	if (!sink->bound) {
		/*
		 * The peer has not attached yet, which is ordinary at startup:
		 * the two cores are started separately and the far half of the
		 * pipeline may not exist for another second.
		 *
		 * Report success and drop the buffer. Returning an error here
		 * fails the push, and a tee propagates that to the whole
		 * pipeline -- so a branch that is merely not ready yet would
		 * stop the branches that are, including the one making the
		 * audio audible.
		 */
		sink->dropped++;
		net_buf_unref(in_buf);
		return 0;
	}

	for (int i = 0; i < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS; i++) {
		if (sink->pending[i] == NULL) {
			id = i;
			break;
		}
	}

	if (id < 0) {
		/*
		 * Every slot is with the peer. Drop this buffer rather than
		 * block: this runs on the pipeline's own thread, and stalling
		 * here stalls the capture feeding it -- including any branch
		 * that is not waiting on the peer at all.
		 */
		sink->dropped++;
		net_buf_unref(in_buf);
		return 0;
	}

	/*
	 * Hold the reference for as long as the peer might read the samples.
	 * The buffer is released when the peer says it is done, not when this
	 * function returns.
	 */
	sink->pending[id] = net_buf_ref(in_buf);

	if (IS_ENABLED(CONFIG_CACHE_MANAGEMENT)) {
		sys_cache_data_flush_range(in_buf->data, meta->bytes_used);
	}

	msg = (struct mpipe_ipc_msg){
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.phys_addr = (uint32_t)mpipe_ipc_virt_to_phys(in_buf->data),
			.size = meta->bytes_used,
			.timestamp = meta->timestamp,
			.buffer_id = (uint32_t)id,
		},
	};

	ret = sink_send(sink, &msg);
	if (ret != 0) {
		net_buf_unref(sink->pending[id]);
		sink->pending[id] = NULL;
		net_buf_unref(in_buf);
		LOG_ERR("cannot hand over buffer %d: %d", id, ret);
		return ret;
	}

	net_buf_unref(in_buf);

	return 0;
}

/*
 * Tell the peer what the pipeline settled on, instead of letting it be
 * configured separately at the other end where the two can disagree silently.
 */
static int sink_set_caps(struct mpipe_sink *base, const struct mpipe_structure *caps)
{
	struct mpipe_ipc_sink *sink = (struct mpipe_ipc_sink *)base;
	struct mpipe_ipc_msg msg = { .type = MPIPE_IPC_MSG_CAPS };
	int ret;

	if (caps == NULL) {
		return -EINVAL;
	}

	/*
	 * Record it on the pad first. That is what the base sink does, and it
	 * is how the negotiation completes; announcing the format to the peer
	 * is additional, not instead.
	 */
	ret = mpipe_pad_set_caps(&sink->base.sink_pad, caps);
	if (ret < 0) {
		return ret;
	}

	sink->caps = *caps;
	sink->have_caps = true;

	if (!sink->bound) {
		/* Sent again on bind; the peer is not there to hear it yet. */
		return 0;
	}

	msg.caps = *caps;

	return sink_send(sink, &msg);
}

/*
 * The end of the stream is part of the stream, so it crosses the link too --
 * and then the base handler still runs. Replacing it rather than extending it
 * loses the pad's caps bookkeeping and the bus message that tells this
 * pipeline its stream ended, which is not a trade worth making to add one
 * forwarding rule.
 */
static int sink_event_fn(struct mpipe_pad *pad, struct mpipe_dispatch *event)
{
	struct mpipe_ipc_sink *sink = CONTAINER_OF(pad->object.container,
						   struct mpipe_ipc_sink,
						   base.element.object);

	if (event == NULL) {
		return -EINVAL;
	}

	if (event->type == MPIPE_DISPATCH_EOS && sink->bound) {
		struct mpipe_ipc_msg msg = {
			.type = MPIPE_IPC_MSG_EVENT,
			.event = { .event_type = MPIPE_DISPATCH_EOS },
		};

		(void)sink_send(sink, &msg);
	}

	return sink->base_event_fn(pad, event);
}

bool mpipe_ipc_sink_is_bound(const struct mpipe_ipc_sink *sink)
{
	return (sink != NULL) && sink->bound;
}

int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct device *instance, const char *name)
{
	int ret;

	if (sink == NULL || instance == NULL || name == NULL) {
		return -EINVAL;
	}

	memset(sink, 0, sizeof(*sink));

	ret = mpipe_sink_init(&sink->base, id);
	if (ret < 0) {
		return ret;
	}

	sink->base.sink_pad.chain_fn = sink_chain_fn;
	sink->base_event_fn = sink->base.sink_pad.event_fn;
	sink->base.sink_pad.event_fn = sink_event_fn;
	sink->base.set_caps = sink_set_caps;

	sink->cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = sink_bound, .received = sink_received },
		.priv = sink,
	};

	return ipc_service_register_endpoint(instance, &sink->ept, &sink->cfg);
}
