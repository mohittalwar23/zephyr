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

#define OPERATION_CLOSED ((atomic_val_t)ATOMIC_MASK(ATOMIC_BITS - 1U))
#define OPERATION_COUNT  (~OPERATION_CLOSED)

static bool region_valid(const struct mpipe_ipc_region *region)
{
	uintptr_t base;

	if (region == NULL || region->base == NULL || region->size == 0U ||
	    region->size > UINT32_MAX || !is_power_of_two(region->align)) {
		return false;
	}

	base = (uintptr_t)region->base;
	return base <= UINTPTR_MAX - region->size && (base & (region->align - 1U)) == 0U;
}

static bool operation_enter(struct mpipe_ipc_sink *sink)
{
	atomic_val_t old;

	do {
		old = atomic_get(&sink->operation_state);
		if ((old & OPERATION_CLOSED) != 0) {
			return false;
		}
		__ASSERT_NO_MSG((old & OPERATION_COUNT) != OPERATION_COUNT);
	} while (!atomic_cas(&sink->operation_state, old, old + 1));

	return true;
}

static void operation_leave(struct mpipe_ipc_sink *sink)
{
	atomic_val_t old = atomic_dec(&sink->operation_state);

	__ASSERT_NO_MSG((old & OPERATION_COUNT) > 0);
	if ((old & OPERATION_COUNT) == 1 && (old & OPERATION_CLOSED) != 0) {
		k_sem_give(&sink->operations_drained);
	}
}

static void operation_close(struct mpipe_ipc_sink *sink)
{
	atomic_val_t old = atomic_or(&sink->operation_state, OPERATION_CLOSED);

	if ((old & OPERATION_COUNT) != 0) {
		(void)k_sem_take(&sink->operations_drained, K_FOREVER);
	}
}

static bool session_current(struct mpipe_ipc_sink *sink)
{
	return sink->transport != NULL &&
	       sink->transport->state == MPIPE_IPC_TRANSPORT_RUNNING &&
	       mpipe_ipc_transport_check_peer(sink->transport) == 0;
}

static int sink_buffer_offset(const struct mpipe_ipc_sink *sink, const struct net_buf *buf,
			      size_t size, uint32_t *offset)
{
	uintptr_t base = (uintptr_t)sink->region.base;
	uintptr_t address = (uintptr_t)buf->data;
	size_t delta;

	if (size == 0U || size > buf->len || address < base) {
		return -ERANGE;
	}

	delta = address - base;
	if (delta > sink->region.size || size > sink->region.size - delta ||
	    (delta & (sink->region.align - 1U)) != 0U ||
	    (size & (sink->region.align - 1U)) != 0U) {
		return -ERANGE;
	}

	*offset = (uint32_t)delta;
	return 0;
}

static int sink_send(struct mpipe_ipc_sink *sink, const struct mpipe_ipc_msg *msg)
{
	int ret;

	if (atomic_get(&sink->registered) == 0 || !session_current(sink)) {
		return -ENOTCONN;
	}

	ret = ipc_service_send(&sink->ept, msg, sizeof(*msg));
	if (ret < 0) {
		return ret;
	}

	return ret == sizeof(*msg) ? 0 : -EMSGSIZE;
}

static void sink_received(const void *data, size_t len, void *priv)
{
	struct mpipe_ipc_sink *sink = priv;
	const struct mpipe_ipc_msg *msg = data;
	struct net_buf *buf = NULL;
	k_spinlock_key_t key;
	uint32_t id;

	if (!operation_enter(sink)) {
		return;
	}
	if (!session_current(sink)) {
		goto out;
	}
	if (len != sizeof(*msg)) {
		LOG_ERR("message of %zu bytes, expected %zu", len, sizeof(*msg));
		goto out;
	}
	if (msg->version != MPIPE_IPC_PLUGIN_VERSION) {
		LOG_ERR("message version %u, expected %u", msg->version,
			MPIPE_IPC_PLUGIN_VERSION);
		goto out;
	}
	if (msg->type != MPIPE_IPC_MSG_DATA_RELEASE) {
		LOG_DBG("ignoring message type %u", msg->type);
		goto out;
	}

	id = msg->release.buffer_id;
	if (id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS) {
		LOG_ERR("release buffer id %u is out of range", id);
		goto out;
	}

	key = k_spin_lock(&sink->pending_lock);
	if (sink->pending[id] != NULL &&
	    msg->release.generation == sink->pending_generation[id]) {
		buf = sink->pending[id];
		sink->pending[id] = NULL;
		sink->pending_generation[id] = MPIPE_IPC_SID_NONE;
	}
	k_spin_unlock(&sink->pending_lock, key);

	if (buf == NULL) {
		LOG_ERR("release for buffer %u generation %u is not outstanding", id,
			msg->release.generation);
		goto out;
	}

	net_buf_unref(buf);

out:
	operation_leave(sink);
}

static void sink_bound(void *priv)
{
	struct mpipe_ipc_sink *sink = priv;
	struct mpipe_ipc_msg msg;
	k_spinlock_key_t key;
	bool send_caps = false;

	if (!operation_enter(sink)) {
		return;
	}
	if (!session_current(sink)) {
		goto out;
	}

	atomic_set(&sink->bound, 1);
	LOG_INF("peer source bound");

	key = k_spin_lock(&sink->caps_lock);
	if (atomic_get(&sink->have_caps) != 0) {
		msg = (struct mpipe_ipc_msg){
			.version = MPIPE_IPC_PLUGIN_VERSION,
			.type = MPIPE_IPC_MSG_CAPS,
			.caps = sink->caps,
		};
		send_caps = true;
	}
	k_spin_unlock(&sink->caps_lock, key);
	if (send_caps) {
		(void)sink_send(sink, &msg);
	}

out:
	operation_leave(sink);
}

static int sink_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
			 struct net_buf **out_buf)
{
	struct mpipe_ipc_sink *sink;
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_msg msg;
	struct net_buf *held = NULL;
	k_spinlock_key_t key;
	uint16_t generation;
	uint32_t offset;
	int id = -1;
	int ret;

	__ASSERT_NO_MSG(pad != NULL);
	__ASSERT_NO_MSG(in_buf != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	sink = CONTAINER_OF(pad->object.container, struct mpipe_ipc_sink,
			    base.element.object);
	meta = mpipe_buffer_get_meta(in_buf);
	*out_buf = NULL;

	if (!operation_enter(sink)) {
		net_buf_unref(in_buf);
		return -ESHUTDOWN;
	}
	if (!session_current(sink)) {
		ret = -ECONNRESET;
		goto drop;
	}

	ret = sink_buffer_offset(sink, in_buf, meta->bytes_used, &offset);
	if (ret != 0) {
		LOG_ERR("buffer is outside the shared payload region");
		goto drop;
	}

	if (atomic_get(&sink->bound) == 0) {
		ret = 0;
		goto drop;
	}

	generation = sink->transport->session.local_sid;
	key = k_spin_lock(&sink->pending_lock);
	for (int i = 0; i < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS; i++) {
		if (sink->pending[i] == NULL) {
			id = i;
			held = net_buf_ref(in_buf);
			sink->pending[i] = held;
			sink->pending_generation[i] = generation;
			break;
		}
	}
	k_spin_unlock(&sink->pending_lock, key);

	if (id < 0) {
		ret = 0;
		goto drop;
	}

	if (IS_ENABLED(CONFIG_CACHE_MANAGEMENT)) {
		sys_cache_data_flush_range(in_buf->data, meta->bytes_used);
	}

	msg = (struct mpipe_ipc_msg){
		.version = MPIPE_IPC_PLUGIN_VERSION,
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = offset,
			.size = meta->bytes_used,
			.timestamp = meta->timestamp,
			.buffer_id = (uint32_t)id,
			.generation = generation,
		},
	};

	ret = sink_send(sink, &msg);
	if (ret != 0) {
		bool detached = false;

		key = k_spin_lock(&sink->pending_lock);
		if (sink->pending[id] == held &&
		    sink->pending_generation[id] == generation) {
			sink->pending[id] = NULL;
			sink->pending_generation[id] = MPIPE_IPC_SID_NONE;
			detached = true;
		}
		k_spin_unlock(&sink->pending_lock, key);
		if (detached) {
			net_buf_unref(held);
		}
		LOG_ERR("cannot hand over buffer %d: %d", id, ret);
	}

	net_buf_unref(in_buf);
	operation_leave(sink);
	return ret;

drop:
	atomic_inc(&sink->dropped);
	net_buf_unref(in_buf);
	operation_leave(sink);
	return ret;
}

static int sink_set_caps(struct mpipe_sink *base, const struct mpipe_structure *caps)
{
	struct mpipe_ipc_sink *sink = (struct mpipe_ipc_sink *)base;
	struct mpipe_ipc_msg msg = {
		.version = MPIPE_IPC_PLUGIN_VERSION,
		.type = MPIPE_IPC_MSG_CAPS,
	};
	k_spinlock_key_t key;
	int ret;

	if (caps == NULL) {
		return -EINVAL;
	}

	ret = mpipe_pad_set_caps(&sink->base.sink_pad, caps);
	if (ret < 0) {
		return ret;
	}

	key = k_spin_lock(&sink->caps_lock);
	sink->caps = *caps;
	atomic_set(&sink->have_caps, 1);
	k_spin_unlock(&sink->caps_lock, key);
	if (atomic_get(&sink->bound) == 0) {
		return 0;
	}
	if (!operation_enter(sink)) {
		return -ESHUTDOWN;
	}

	msg.caps = *caps;
	ret = sink_send(sink, &msg);
	operation_leave(sink);

	return ret;
}

static int sink_event_fn(struct mpipe_pad *pad, struct mpipe_dispatch *event)
{
	struct mpipe_ipc_sink *sink = CONTAINER_OF(pad->object.container,
						   struct mpipe_ipc_sink,
						   base.element.object);

	if (event == NULL) {
		return -EINVAL;
	}

	if (event->type == MPIPE_DISPATCH_EOS && atomic_get(&sink->bound) != 0 &&
	    operation_enter(sink)) {
		struct mpipe_ipc_msg msg = {
			.version = MPIPE_IPC_PLUGIN_VERSION,
			.type = MPIPE_IPC_MSG_EVENT,
			.event = { .event_type = MPIPE_DISPATCH_EOS },
		};

		(void)sink_send(sink, &msg);
		operation_leave(sink);
	}

	return sink->base_event_fn(pad, event);
}

static int sink_propose_buffer_pool(struct mpipe_sink *base,
				    struct mpipe_dispatch *query)
{
	ARG_UNUSED(base);

	query->pool = NULL;
	query->pool_cfg = (struct mpipe_buffer_pool_config){
		.min_buffers = CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS,
	};

	return 0;
}

bool mpipe_ipc_sink_is_bound(const struct mpipe_ipc_sink *sink)
{
	return sink != NULL && atomic_get(&sink->bound) != 0;
}

int mpipe_ipc_sink_deinit(struct mpipe_ipc_sink *sink)
{
	int ret;

	if (sink == NULL) {
		return -EINVAL;
	}

	atomic_set(&sink->bound, 0);
	operation_close(sink);
	if (atomic_get(&sink->registered) == 0) {
		return 0;
	}

	ret = ipc_service_deregister_endpoint(&sink->ept);
	if (ret == 0) {
		atomic_set(&sink->registered, 0);
	}

	return ret;
}

int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct device *instance, const char *name,
			const struct mpipe_ipc_region *region,
			struct mpipe_ipc_transport *transport)
{
	int ret;

	if (sink == NULL || instance == NULL || name == NULL || !region_valid(region) ||
	    transport == NULL ||
	    (transport->ops == &mpipe_ipc_zephyr_ops && transport->context != instance)) {
		return -EINVAL;
	}
	if (transport->state != MPIPE_IPC_TRANSPORT_RUNNING || !transport->opened ||
	    !transport->session.connected) {
		return -ENOTCONN;
	}
	ret = mpipe_ipc_transport_check_peer(transport);
	if (ret != 0) {
		return ret;
	}

	memset(sink, 0, sizeof(*sink));
	sink->region = *region;
	sink->transport = transport;
	k_sem_init(&sink->operations_drained, 0, 1);

	ret = mpipe_sink_init(&sink->base, id);
	if (ret < 0) {
		return ret;
	}

	sink->base.sink_pad.chain_fn = sink_chain_fn;
	sink->base_event_fn = sink->base.sink_pad.event_fn;
	sink->base.sink_pad.event_fn = sink_event_fn;
	sink->base.set_caps = sink_set_caps;
	sink->base.propose_buffer_pool = sink_propose_buffer_pool;
	sink->cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = sink_bound, .received = sink_received },
		.priv = sink,
	};

	atomic_set(&sink->operation_state, 0);
	ret = ipc_service_register_endpoint(instance, &sink->ept, &sink->cfg);
	if (ret != 0) {
		atomic_set(&sink->operation_state, OPERATION_CLOSED);
		return ret;
	}
	atomic_set(&sink->registered, 1);

	return 0;
}
