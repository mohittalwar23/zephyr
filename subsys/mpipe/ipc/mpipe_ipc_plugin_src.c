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
#include <zephyr/mpipe/mpipe_pipeline.h>

LOG_MODULE_REGISTER(mpipe_ipc_plugin_src, CONFIG_MPIPE_LOG_LEVEL);

/*
 * Wrappers, not storage.
 *
 * Each buffer here describes memory the peer owns, so the pool is defined with
 * no payload of its own: the samples stay where the peer wrote them and are
 * read in place.
 */
NET_BUF_POOL_FIXED_DEFINE(ipc_src_wrappers, MPIPE_IPC_MAX_BUFFERS, 0,
			  sizeof(struct mpipe_buffer_meta), NULL);

/* The source owning the pool, so a release can be sent from the pool hook. */
static struct mpipe_ipc_src *pool_owner;

/*
 * Called when the last downstream reference to a wrapper is dropped. That is
 * the moment -- and the only moment -- at which the peer may reuse the memory,
 * so this is where the release goes out.
 */
static int wrapper_released(struct mpipe_buffer_pool *pool, struct net_buf *buf)
{
	struct mpipe_ipc_src *src = pool_owner;
	struct mpipe_buffer_meta *meta = mpipe_buffer_get_meta(buf);
	struct mpipe_ipc_msg msg;
	int ret;

	ARG_UNUSED(pool);

	if (src == NULL || !src->bound) {
		return 0;
	}

	msg = (struct mpipe_ipc_msg){
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = (uint32_t)(uintptr_t)meta->priv },
	};

	ret = ipc_service_send(&src->ept, &msg, sizeof(msg));
	if (ret < 0) {
		/*
		 * The peer now has a buffer it will never get back, and will
		 * run out of slots. Nothing here can recover it, so say so
		 * rather than let the stall look like a capture problem later.
		 */
		LOG_ERR("cannot release buffer %u to the peer: %d",
			(uint32_t)(uintptr_t)meta->priv, ret);
	}

	return 0;
}

static struct mpipe_buffer_pool wrapper_pool = {
	.nb_pool = &ipc_src_wrappers,
	.release_buffer = wrapper_released,
};

static void src_bound(void *priv)
{
	struct mpipe_ipc_src *src = priv;

	src->bound = true;
	LOG_INF("peer sink bound");
}

static void return_buffer(struct mpipe_ipc_src *src, uint32_t buffer_id)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = buffer_id },
	};

	(void)ipc_service_send(&src->ept, &msg, sizeof(msg));
}

static void src_received(const void *data, size_t len, void *priv)
{
	struct mpipe_ipc_src *src = priv;
	const struct mpipe_ipc_msg *msg = data;
	struct mpipe_buffer_meta *meta;
	struct net_buf *buf;

	if (len != sizeof(*msg)) {
		LOG_ERR("message of %zu bytes, expected %zu", len, sizeof(*msg));
		return;
	}

	if (msg->type != MPIPE_IPC_MSG_DATA_BUFFER) {
		LOG_DBG("ignoring message type %u", msg->type);
		return;
	}

	buf = net_buf_alloc(&ipc_src_wrappers, K_NO_WAIT);
	if (buf == NULL) {
		/*
		 * Hand it straight back. Holding a buffer this core cannot
		 * describe would strand a slot on the peer for good.
		 */
		src->refused++;
		return_buffer(src, msg->data.buffer_id);
		return;
	}

	/* Point at the peer's memory rather than copying out of it. */
	buf->data = mpipe_ipc_phys_to_virt(msg->data.phys_addr);
	buf->len = msg->data.size;
	buf->size = msg->data.size;

	if (IS_ENABLED(CONFIG_CACHE_MANAGEMENT)) {
		sys_cache_data_invd_range(buf->data, buf->len);
	}

	meta = mpipe_buffer_get_meta(buf);
	meta->pool = &wrapper_pool;
	meta->bytes_used = msg->data.size;
	meta->timestamp = msg->data.timestamp;
	/* Carried so the release can name the buffer the peer knows. */
	meta->priv = (void *)(uintptr_t)msg->data.buffer_id;

	(void)mpipe_push_buffer(&src->base.src_pad, buf);
}

int mpipe_ipc_src_set_format(struct mpipe_ipc_src *src,
			     const struct mpipe_structure *caps)
{
	if (src == NULL || caps == NULL) {
		return -EINVAL;
	}

	return mpipe_pad_set_caps(&src->base.src_pad, caps);
}

bool mpipe_ipc_src_is_bound(const struct mpipe_ipc_src *src)
{
	return (src != NULL) && src->bound;
}

int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct device *instance, const char *name)
{
	struct ipc_ept_cfg cfg;
	int ret;

	if (src == NULL || instance == NULL || name == NULL) {
		return -EINVAL;
	}

	memset(src, 0, sizeof(*src));

	ret = mpipe_src_init(&src->base, id);
	if (ret < 0) {
		return ret;
	}

	src->base.pool = &wrapper_pool;
	pool_owner = src;

	cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = src_bound, .received = src_received },
		.priv = src,
	};

	return ipc_service_register_endpoint(instance, &src->ept, &cfg);
}
