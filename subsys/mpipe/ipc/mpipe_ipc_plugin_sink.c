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

LOG_MODULE_REGISTER(mpipe_ipc_plugin_sink, CONFIG_MPIPE_LOG_LEVEL);

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
	if (id >= MPIPE_IPC_MAX_BUFFERS || sink->pending[id] == NULL) {
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
		net_buf_unref(in_buf);
		return -ENOTCONN;
	}

	for (int i = 0; i < MPIPE_IPC_MAX_BUFFERS; i++) {
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

	ret = ipc_service_send(&sink->ept, &msg, sizeof(msg));
	if (ret < 0) {
		net_buf_unref(sink->pending[id]);
		sink->pending[id] = NULL;
		net_buf_unref(in_buf);
		LOG_ERR("cannot hand over buffer %d: %d", id, ret);
		return ret;
	}

	net_buf_unref(in_buf);

	return 0;
}

bool mpipe_ipc_sink_is_bound(const struct mpipe_ipc_sink *sink)
{
	return (sink != NULL) && sink->bound;
}

int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct device *instance, const char *name)
{
	struct ipc_ept_cfg cfg;
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

	cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = sink_bound, .received = sink_received },
		.priv = sink,
	};

	return ipc_service_register_endpoint(instance, &sink->ept, &cfg);
}
