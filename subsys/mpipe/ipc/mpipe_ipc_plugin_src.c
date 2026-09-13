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
#include <zephyr/mpipe/mpipe_pipeline.h>

LOG_MODULE_REGISTER(mpipe_ipc_plugin_src, CONFIG_MPIPE_LOG_LEVEL);

/*
 * Wrappers, not storage.
 *
 * Each buffer here describes memory the peer owns, so the pool is defined with
 * no payload of its own: the samples stay where the peer wrote them and are
 * read in place.
 */
NET_BUF_POOL_FIXED_DEFINE(ipc_src_wrappers, CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS, 0,
			  sizeof(struct mpipe_buffer_meta), mpipe_buffer_destroy);

/* The source owning the pool, so a release can be sent from the pool hook. */
static struct mpipe_ipc_src *pool_owner;

/*
 * Called when the last downstream reference to a wrapper is dropped. That is
 * the moment -- and the only moment -- at which the peer may reuse the memory,
 * so this is where the release goes out.
 */
static void return_buffer(struct mpipe_ipc_src *src, uint32_t buffer_id);

static int wrapper_released(struct mpipe_buffer_pool *pool, struct net_buf *buf)
{
	struct mpipe_ipc_src *src = pool_owner;
	struct mpipe_buffer_meta *meta = mpipe_buffer_get_meta(buf);

	ARG_UNUSED(pool);

	if (src == NULL || !src->bound) {
		return 0;
	}

	return_buffer(src, (uint32_t)(uintptr_t)meta->priv);

	return 0;
}

/*
 * This pool owns no memory, so the lifecycle hooks have nothing to do -- but
 * they cannot be absent. The pipeline calls start, stop and the configuration
 * hooks on whatever pool an element advertises, and a NULL there is a jump to a
 * garbage address rather than a graceful refusal.
 */
static int wrapper_pool_noop(struct mpipe_buffer_pool *pool)
{
	ARG_UNUSED(pool);

	return 0;
}

static int wrapper_pool_configure(struct mpipe_buffer_pool *pool,
				  struct mpipe_structure *config)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(config);

	return 0;
}

static int wrapper_pool_set_config(struct mpipe_buffer_pool *pool,
				   const struct mpipe_buffer_pool_config *cfg)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(cfg);

	return 0;
}

static int wrapper_pool_acquire(struct mpipe_buffer_pool *pool, struct net_buf **buf)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(buf);

	/*
	 * Buffers here arrive from the peer; there is nothing to hand out on
	 * request, and a caller that asks has misunderstood the element.
	 */
	return -ENOTSUP;
}

static struct mpipe_buffer_pool wrapper_pool = {
	.nb_pool = &ipc_src_wrappers,
	.configure = wrapper_pool_configure,
	.set_config = wrapper_pool_set_config,
	.start = wrapper_pool_noop,
	.stop = wrapper_pool_noop,
	.acquire_buffer = wrapper_pool_acquire,
	.release_buffer = wrapper_released,
};

static void src_bound(void *priv)
{
	struct mpipe_ipc_src *src = priv;

	src->bound = true;
	LOG_INF("peer sink bound");
}

static int src_send(struct mpipe_ipc_src *src, const struct mpipe_ipc_msg *msg)
{
	int ret = ipc_service_send(&src->ept, msg, sizeof(*msg));

	if (ret < 0) {
		return ret;
	}

	return ret == sizeof(*msg) ? 0 : -EMSGSIZE;
}

/*
 * A release that is not delivered is a buffer the peer never gets back.
 *
 * The control channel can refuse a send when its transmit buffers are
 * momentarily exhausted, which under load is ordinary rather than exceptional.
 * Dropping the release there leaks one of the peer's slots permanently, and
 * enough of those starve the pipeline feeding the link -- slowly, so it looks
 * like a throughput problem rather than a lost message. Failed releases are
 * therefore remembered and retried.
 */
static void flush_releases(struct mpipe_ipc_src *src)
{
	for (unsigned int i = 0; i < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS; i++) {
		struct mpipe_ipc_msg msg = {
			.type = MPIPE_IPC_MSG_DATA_RELEASE,
			.release = { .buffer_id = i },
		};

		if (!atomic_test_bit(src->unreleased, i)) {
			continue;
		}

		if (src_send(src, &msg) != 0) {
			return;
		}

		atomic_clear_bit(src->unreleased, i);
	}
}

static void return_buffer(struct mpipe_ipc_src *src, uint32_t buffer_id)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = buffer_id },
	};

	if (buffer_id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS) {
		return;
	}

	flush_releases(src);

	if (src_send(src, &msg) != 0) {
		atomic_set_bit(src->unreleased, buffer_id);
		src->deferred++;
	}
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

	flush_releases(src);

	if (msg->type == MPIPE_IPC_MSG_CAPS) {
		/*
		 * The format the peer's half settled on. Applying it here is
		 * what lets one negotiation cover both halves, instead of each
		 * core being configured separately and trusted to match.
		 */
		struct mpipe_dispatch event = {
			.type = MPIPE_DISPATCH_CAPS,
			.caps = &src->caps,
		};

		src->caps = msg->caps;
		src->have_caps = true;

		(void)mpipe_pad_set_caps(&src->base.src_pad, &src->caps);

		if (src->base.src_pad.peer != NULL) {
			(void)mpipe_pad_send_event(src->base.src_pad.peer, &event);
		}
		return;
	}

	if (msg->type == MPIPE_IPC_MSG_EVENT) {
		if (msg->event.event_type == MPIPE_DISPATCH_EOS &&
		    src->base.src_pad.peer != NULL) {
			struct mpipe_dispatch event = { .type = MPIPE_DISPATCH_EOS };

			/* The stream ended on the other core; say so downstream. */
			(void)mpipe_pad_send_event(src->base.src_pad.peer, &event);
		}
		return;
	}

	if (msg->type != MPIPE_IPC_MSG_DATA_BUFFER) {
		LOG_DBG("ignoring message type %u", msg->type);
		return;
	}

	/*
	 * Registering the endpoint is what makes the peer start sending, before
	 * the graph necessarily reaches PLAYING. The source lifecycle gate opens
	 * only in PLAYING and drains on pause, so hand back anything that arrives
	 * outside that interval.
	 */
	if (msg->data.buffer_id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS) {
		LOG_ERR("buffer id %u is out of range", msg->data.buffer_id);
		return;
	}

	if (!mpipe_src_delivery_enter(&src->base)) {
		return_buffer(src, msg->data.buffer_id);
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
		mpipe_src_delivery_leave(&src->base);
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

	/* mpipe_push_buffer() consumes the reference on both success and failure. */
	(void)mpipe_push_buffer(&src->base.src_pad, buf);
	mpipe_src_delivery_leave(&src->base);
}

int mpipe_ipc_src_set_format(struct mpipe_ipc_src *src,
			     const struct mpipe_structure *caps)
{
	if (src == NULL || caps == NULL) {
		return -EINVAL;
	}

	return mpipe_pad_set_caps(&src->base.src_pad, caps);
}

bool mpipe_ipc_src_has_caps(const struct mpipe_ipc_src *src)
{
	return (src != NULL) && src->have_caps;
}

bool mpipe_ipc_src_is_bound(const struct mpipe_ipc_src *src)
{
	return (src != NULL) && src->bound;
}

int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct device *instance, const char *name)
{
	int ret;

	if (src == NULL || instance == NULL || name == NULL) {
		return -EINVAL;
	}

	memset(src, 0, sizeof(*src));

	ret = mpipe_src_init(&src->base, id);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Buffers arrive when the peer sends them, so this source is not
	 * something the pipeline can pull from.
	 */
	src->base.drive = MPIPE_SRC_DRIVE_PUSH;
	src->base.pool = &wrapper_pool;
	pool_owner = src;

	src->cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = src_bound, .received = src_received },
		.priv = src,
	};

	return ipc_service_register_endpoint(instance, &src->ept, &src->cfg);
}
