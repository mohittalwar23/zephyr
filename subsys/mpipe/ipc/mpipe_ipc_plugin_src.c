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

#define RELEASE_RETRY_DELAY K_MSEC(10)
#define WRAPPER_ID_BITS     8U
#define WRAPPER_ID_MASK     BIT_MASK(WRAPPER_ID_BITS)
#define OPERATION_CLOSED    ((atomic_val_t)ATOMIC_MASK(ATOMIC_BITS - 1U))
#define OPERATION_COUNT     (~OPERATION_CLOSED)

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

static bool caps_field_valid(uint8_t media_type, uint8_t field_id)
{
	if (field_id == MPIPE_CAPS_FRAME_INTERVAL) {
		return media_type == MPIPE_MEDIA_AUDIO_PCM || media_type == MPIPE_MEDIA_VIDEO;
	}
	if (media_type == MPIPE_MEDIA_AUDIO_PCM) {
		return IN_RANGE(field_id, MPIPE_CAPS_SAMPLE_RATE, MPIPE_CAPS_INTERLEAVED);
	}
	if (media_type == MPIPE_MEDIA_VIDEO) {
		return IN_RANGE(field_id, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_CAPS_IMAGE_HEIGHT);
	}

	return false;
}

static bool caps_value_valid(uint8_t field_id, const struct mpipe_value *value)
{
	if (field_id == MPIPE_CAPS_INTERLEAVED) {
		uint8_t raw_boolean;

		memcpy(&raw_boolean, &value->v_boolean, sizeof(raw_boolean));
		return value->type == MPIPE_TYPE_BOOLEAN && raw_boolean <= 1U;
	}
	if (field_id == MPIPE_CAPS_PIXEL_FORMAT) {
		return value->type == MPIPE_TYPE_UINT;
	}
	if (value->type == MPIPE_TYPE_UINT) {
		return true;
	}
	if (value->type != MPIPE_TYPE_UINT_RANGE) {
		return false;
	}

	return value->range.min.v_uint <= value->range.max.v_uint &&
	       value->range.step.v_uint != 0U;
}

static bool caps_wire_valid(const struct mpipe_structure *caps)
{
	uint32_t fields_seen = 0U;

	if (caps->media_type_id >= MPIPE_MEDIA_END ||
	    (caps->flags & ~MPIPE_STRUCTURE_FLAG_ANY) != 0U ||
	    caps->num_fields > CONFIG_MPIPE_STRUCTURE_MAX_FIELDS) {
		return false;
	}
	if ((caps->flags & MPIPE_STRUCTURE_FLAG_ANY) != 0U) {
		return caps->media_type_id == MPIPE_MEDIA_UNKNOWN && caps->num_fields == 0U;
	}
	if (caps->media_type_id == MPIPE_MEDIA_UNKNOWN) {
		return caps->num_fields == 0U;
	}

	for (uint8_t i = 0; i < caps->num_fields; i++) {
		uint8_t field_id = caps->ids[i];

		if (field_id >= MPIPE_CAPS_END ||
		    !caps_field_valid(caps->media_type_id, field_id) ||
		    (fields_seen & BIT(field_id)) != 0U ||
		    !caps_value_valid(field_id, &caps->values[i])) {
			return false;
		}
		fields_seen |= BIT(field_id);
	}

	return true;
}

static bool src_buffer_valid(const struct mpipe_ipc_src *src, uint32_t offset, uint32_t size)
{
	return size != 0U && offset <= src->region.size && size <= src->region.size - offset &&
	       (offset & (src->region.align - 1U)) == 0U &&
	       (size & (src->region.align - 1U)) == 0U;
}

static bool operation_enter(struct mpipe_ipc_src *src)
{
	atomic_val_t old;

	do {
		old = atomic_get(&src->operation_state);
		if ((old & OPERATION_CLOSED) != 0) {
			return false;
		}
		__ASSERT_NO_MSG((old & OPERATION_COUNT) != OPERATION_COUNT);
	} while (!atomic_cas(&src->operation_state, old, old + 1));

	return true;
}

static void operation_leave(struct mpipe_ipc_src *src)
{
	atomic_val_t old = atomic_dec(&src->operation_state);

	__ASSERT_NO_MSG((old & OPERATION_COUNT) > 0);
	if ((old & OPERATION_COUNT) == 1 && (old & OPERATION_CLOSED) != 0) {
		k_sem_give(&src->operations_drained);
	}
}

static void operation_close(struct mpipe_ipc_src *src)
{
	atomic_val_t old = atomic_or(&src->operation_state, OPERATION_CLOSED);

	if ((old & OPERATION_COUNT) != 0) {
		(void)k_sem_take(&src->operations_drained, K_FOREVER);
	}
}

static bool session_current(struct mpipe_ipc_src *src)
{
	return src->transport != NULL &&
	       src->transport->state == MPIPE_IPC_TRANSPORT_RUNNING &&
	       mpipe_ipc_transport_check_peer(src->transport) == 0;
}

/*
 * Wrappers, not storage. Every wrapper's metadata names its source's own
 * mpipe_buffer_pool, so the common allocation pool does not imply common
 * ownership.
 */
NET_BUF_POOL_FIXED_DEFINE(ipc_src_wrappers, CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS, 0,
			  sizeof(struct mpipe_buffer_meta), mpipe_buffer_destroy);

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
	return -ENOTSUP;
}

static int src_send(struct mpipe_ipc_src *src, const struct mpipe_ipc_msg *msg)
{
	int ret;

	if (atomic_get(&src->registered) == 0 || !session_current(src)) {
		return -ENOTCONN;
	}

	ret = ipc_service_send(&src->ept, msg, sizeof(*msg));
	if (ret < 0) {
		return ret;
	}

	return ret == sizeof(*msg) ? 0 : -EMSGSIZE;
}

static bool release_claim(struct mpipe_ipc_src *src, unsigned int id,
			  uint16_t *generation)
{
	k_spinlock_key_t key = k_spin_lock(&src->release_lock);
	bool pending = atomic_test_bit(src->unreleased, id);

	if (pending) {
		*generation = src->unreleased_generation[id];
		atomic_clear_bit(src->unreleased, id);
	}
	k_spin_unlock(&src->release_lock, key);

	return pending;
}

static void release_restore(struct mpipe_ipc_src *src, unsigned int id,
			    uint16_t generation)
{
	k_spinlock_key_t key = k_spin_lock(&src->release_lock);

	if (!atomic_test_bit(src->unreleased, id)) {
		src->unreleased_generation[id] = generation;
		atomic_set_bit(src->unreleased, id);
	}
	k_spin_unlock(&src->release_lock, key);
}

static bool releases_pending(struct mpipe_ipc_src *src)
{
	for (unsigned int i = 0; i < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS; i++) {
		if (atomic_test_bit(src->unreleased, i)) {
			return true;
		}
	}

	return false;
}

static int send_release(struct mpipe_ipc_src *src, uint32_t buffer_id,
			uint16_t generation)
{
	struct mpipe_ipc_msg msg = {
		.version = MPIPE_IPC_PLUGIN_VERSION,
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = {
			.buffer_id = buffer_id,
			.generation = generation,
		},
	};

	return src_send(src, &msg);
}

static void schedule_release_retry(struct mpipe_ipc_src *src)
{
	if ((atomic_get(&src->operation_state) & OPERATION_CLOSED) == 0) {
		(void)k_work_reschedule(&src->release_work, RELEASE_RETRY_DELAY);
	}
}

static void flush_releases(struct mpipe_ipc_src *src)
{
	for (unsigned int i = 0; i < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS; i++) {
		uint16_t generation;

		if (!release_claim(src, i, &generation)) {
			continue;
		}

		/* Never deliver an old owner's release into a new transport session. */
		if (generation != src->transport->session.remote_sid) {
			continue;
		}
		if (send_release(src, i, generation) != 0) {
			release_restore(src, i, generation);
			atomic_inc(&src->deferred);
			schedule_release_retry(src);
			return;
		}
	}
}

static void queue_release(struct mpipe_ipc_src *src, uint32_t buffer_id,
			  uint16_t generation)
{
	k_spinlock_key_t key = k_spin_lock(&src->release_lock);

	if (!atomic_test_bit(src->unreleased, buffer_id)) {
		src->unreleased_generation[buffer_id] = generation;
		atomic_set_bit(src->unreleased, buffer_id);
	}
	k_spin_unlock(&src->release_lock, key);
}

static void return_buffer(struct mpipe_ipc_src *src, uint32_t buffer_id,
			  uint16_t generation)
{
	if (buffer_id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS ||
	    generation != src->transport->session.remote_sid) {
		return;
	}

	queue_release(src, buffer_id, generation);
	flush_releases(src);
}

static int wrapper_released(struct mpipe_buffer_pool *pool, struct net_buf *buf)
{
	struct mpipe_ipc_src *src = CONTAINER_OF(pool, struct mpipe_ipc_src, wrapper_pool);
	struct mpipe_buffer_meta *meta = mpipe_buffer_get_meta(buf);
	uintptr_t owner = (uintptr_t)meta->priv;
	uint32_t buffer_id = owner & WRAPPER_ID_MASK;
	uint16_t generation = (uint16_t)(owner >> WRAPPER_ID_BITS);

	if (buffer_id < CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS) {
		atomic_clear_bit(src->live, buffer_id);
	}

	if (operation_enter(src)) {
		return_buffer(src, buffer_id, generation);
		operation_leave(src);
	}

	return 0;
}

static void release_work_handler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	struct mpipe_ipc_src *src = CONTAINER_OF(delayable, struct mpipe_ipc_src,
						release_work);

	if (!operation_enter(src)) {
		return;
	}

	flush_releases(src);
	if (releases_pending(src)) {
		schedule_release_retry(src);
	}
	operation_leave(src);
}

static void src_bound(void *priv)
{
	struct mpipe_ipc_src *src = priv;

	if (!operation_enter(src)) {
		return;
	}
	if (session_current(src)) {
		atomic_set(&src->bound, 1);
		LOG_INF("peer sink bound");
	}
	operation_leave(src);
}

static void src_received(const void *data, size_t len, void *priv)
{
	struct mpipe_ipc_src *src = priv;
	const struct mpipe_ipc_msg *msg = data;
	struct mpipe_buffer_meta *meta;
	struct net_buf *buf;
	uint16_t generation;

	if (!operation_enter(src)) {
		return;
	}
	if (!session_current(src)) {
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

	flush_releases(src);

	if (msg->type == MPIPE_IPC_MSG_CAPS) {
		struct mpipe_dispatch event = {
			.type = MPIPE_DISPATCH_CAPS,
			.caps = &src->caps,
		};

		if (!caps_wire_valid(&msg->caps)) {
			LOG_ERR("malformed CAPS message");
			goto out;
		}
		src->caps = msg->caps;
		atomic_set(&src->have_caps, 1);
		(void)mpipe_pad_set_caps(&src->base.src_pad, &src->caps);
		if (src->base.src_pad.peer != NULL) {
			(void)mpipe_pad_send_event(src->base.src_pad.peer, &event);
		}
		goto out;
	}

	if (msg->type == MPIPE_IPC_MSG_EVENT) {
		if (msg->event.event_type == MPIPE_DISPATCH_EOS &&
		    src->base.src_pad.peer != NULL) {
			struct mpipe_dispatch event = { .type = MPIPE_DISPATCH_EOS };

			(void)mpipe_pad_send_event(src->base.src_pad.peer, &event);
		}
		goto out;
	}

	if (msg->type != MPIPE_IPC_MSG_DATA_BUFFER) {
		LOG_DBG("ignoring message type %u", msg->type);
		goto out;
	}

	if (msg->data.generation != src->transport->session.remote_sid) {
		LOG_ERR("DATA generation %u, expected %u", msg->data.generation,
			src->transport->session.remote_sid);
		goto out;
	}
	generation = (uint16_t)msg->data.generation;

	if (msg->data.buffer_id >= CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS) {
		LOG_ERR("buffer id %u is out of range", msg->data.buffer_id);
		goto out;
	}
	if (!src_buffer_valid(src, msg->data.offset, msg->data.size)) {
		LOG_ERR("buffer %u is outside the shared payload region", msg->data.buffer_id);
		return_buffer(src, msg->data.buffer_id, generation);
		goto out;
	}
	if (atomic_test_and_set_bit(src->live, msg->data.buffer_id)) {
		LOG_ERR("buffer %u is already live", msg->data.buffer_id);
		goto out;
	}

	if (!mpipe_src_delivery_enter(&src->base)) {
		atomic_clear_bit(src->live, msg->data.buffer_id);
		return_buffer(src, msg->data.buffer_id, generation);
		goto out;
	}

	buf = net_buf_alloc(&ipc_src_wrappers, K_NO_WAIT);
	if (buf == NULL) {
		atomic_inc(&src->refused);
		atomic_clear_bit(src->live, msg->data.buffer_id);
		return_buffer(src, msg->data.buffer_id, generation);
		mpipe_src_delivery_leave(&src->base);
		goto out;
	}

	buf->data = (uint8_t *)src->region.base + msg->data.offset;
	buf->len = msg->data.size;
	buf->size = msg->data.size;
	if (IS_ENABLED(CONFIG_CACHE_MANAGEMENT)) {
		sys_cache_data_invd_range(buf->data, buf->len);
	}

	meta = mpipe_buffer_get_meta(buf);
	meta->pool = &src->wrapper_pool;
	meta->bytes_used = msg->data.size;
	meta->timestamp = msg->data.timestamp;
	meta->priv = (void *)(((uintptr_t)generation << WRAPPER_ID_BITS) |
			     msg->data.buffer_id);

	/* mpipe_push_buffer() consumes the reference on both success and failure. */
	(void)mpipe_push_buffer(&src->base.src_pad, buf);
	mpipe_src_delivery_leave(&src->base);

out:
	operation_leave(src);
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
	return src != NULL && atomic_get(&src->have_caps) != 0;
}

bool mpipe_ipc_src_is_bound(const struct mpipe_ipc_src *src)
{
	return src != NULL && atomic_get(&src->bound) != 0;
}

int mpipe_ipc_src_deinit(struct mpipe_ipc_src *src)
{
	int ret;

	if (src == NULL) {
		return -EINVAL;
	}

	atomic_set(&src->bound, 0);
	operation_close(src);
	(void)k_work_cancel_delayable_sync(&src->release_work, &src->release_sync);

	if (atomic_get(&src->registered) == 0) {
		return 0;
	}

	/* One final attempt after downstream ownership has drained. */
	flush_releases(src);

	ret = ipc_service_deregister_endpoint(&src->ept);
	if (ret == 0) {
		atomic_set(&src->registered, 0);
	}

	return ret;
}

int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct device *instance, const char *name,
		       const struct mpipe_ipc_region *region,
		       struct mpipe_ipc_transport *transport)
{
	int ret;

	if (src == NULL || instance == NULL || name == NULL || !region_valid(region) ||
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

	memset(src, 0, sizeof(*src));
	src->region = *region;
	src->transport = transport;
	k_sem_init(&src->operations_drained, 0, 1);
	k_work_init_delayable(&src->release_work, release_work_handler);

	ret = mpipe_src_init(&src->base, id);
	if (ret < 0) {
		return ret;
	}

	src->wrapper_pool = (struct mpipe_buffer_pool){
		.nb_pool = &ipc_src_wrappers,
		.configure = wrapper_pool_configure,
		.set_config = wrapper_pool_set_config,
		.start = wrapper_pool_noop,
		.stop = wrapper_pool_noop,
		.acquire_buffer = wrapper_pool_acquire,
		.release_buffer = wrapper_released,
	};
	src->base.drive = MPIPE_SRC_DRIVE_PUSH;
	src->base.pool = &src->wrapper_pool;
	src->cfg = (struct ipc_ept_cfg){
		.name = name,
		.cb = { .bound = src_bound, .received = src_received },
		.priv = src,
	};

	atomic_set(&src->operation_state, 0);
	ret = ipc_service_register_endpoint(instance, &src->ept, &src->cfg);
	if (ret != 0) {
		atomic_set(&src->operation_state, OPERATION_CLOSED);
		return ret;
	}
	atomic_set(&src->registered, 1);

	return 0;
}
