/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_plugin.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>
#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_pipeline.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#define DT_DRV_COMPAT zephyr_mpipe_ipc_test
#define FAKE_IPC_NODE DT_NODELABEL(fake_ipc)
#define SEND_FULL INT_MIN

struct fake_ipc_context {
	const struct ipc_ept_cfg *cfg[8];
	/* What was transmitted, decoded back: the wire is what is asserted on. */
	struct mpipe_ipc_msg sent[8];
	int sent_decoded[8];
	size_t sent_len[8];
	unsigned int sent_endpoint[8];
	unsigned int register_count;
	unsigned int deregister_count;
	unsigned int send_count;
	int send_result;
	int deregister_result;
	unsigned int block_send_at;
};

static struct fake_ipc_context fake;
static unsigned int chain_received;
static bool block_consume;
static int async_deinit_result;
static int async_sink_deinit_result;
static uint8_t next_element_id = 1U;
static uint32_t shared_payload[4];
static struct mpipe_ipc_shared session_shared;
static struct mpipe_ipc_transport test_transport;
static const struct mpipe_ipc_region test_region = {
	.base = shared_payload,
	.size = sizeof(shared_payload),
	.align = sizeof(uint32_t),
};

static uint32_t session_load(const volatile uint32_t *address)
{
	return *address;
}

static void session_store(volatile uint32_t *address, uint32_t value)
{
	*address = value;
}

static const struct mpipe_ipc_ops session_ops = {
	.load = session_load,
	.store = session_store,
};

K_SEM_DEFINE(consume_entered, 0, 1);
K_SEM_DEFINE(consume_release, 0, 1);
K_SEM_DEFINE(async_deinit_done, 0, 1);
K_SEM_DEFINE(async_sink_deinit_done, 0, 1);
K_SEM_DEFINE(send_entered, 0, 1);
K_SEM_DEFINE(send_release, 0, 1);
K_SEM_DEFINE(buffer_release_entered, 0, 1);
K_SEM_DEFINE(buffer_release_continue, 0, 1);
K_THREAD_STACK_DEFINE(receive_stack, 2048);
K_THREAD_STACK_DEFINE(deinit_stack, 2048);
K_THREAD_STACK_DEFINE(sink_deinit_stack, 2048);
static struct k_thread receive_thread;
static struct k_thread deinit_thread;
static struct k_thread sink_deinit_thread;

static int fake_register_endpoint(const struct device *instance, void **token,
				  const struct ipc_ept_cfg *cfg)
{
	unsigned int id = fake.register_count++;

	ARG_UNUSED(instance);

	zassert_true(id < ARRAY_SIZE(fake.cfg));
	fake.cfg[id] = cfg;
	*token = (void *)(uintptr_t)(id + 1U);
	return 0;
}

static int fake_deregister_endpoint(const struct device *instance, void *token)
{
	ARG_UNUSED(instance);
	zassert_not_null(token);
	fake.deregister_count++;
	return fake.deregister_result;
}

static int fake_send(const struct device *instance, void *token, const void *data, size_t len)
{
	unsigned int slot = fake.send_count++;

	ARG_UNUSED(instance);
	ARG_UNUSED(token);

	if (slot < ARRAY_SIZE(fake.sent)) {
		fake.sent_decoded[slot] = mpipe_ipc_msg_decode(&fake.sent[slot], data, len);
		fake.sent_len[slot] = len;
		fake.sent_endpoint[slot] = (unsigned int)(uintptr_t)token - 1U;
	}
	if (fake.block_send_at == slot + 1U) {
		k_sem_give(&send_entered);
		(void)k_sem_take(&send_release, K_FOREVER);
	}

	return fake.send_result == SEND_FULL ? (int)len : fake.send_result;
}

static const struct ipc_service_backend fake_backend = {
	.send = fake_send,
	.register_endpoint = fake_register_endpoint,
	.deregister_endpoint = fake_deregister_endpoint,
};

#define DEFINE_FAKE_IPC_DEVICE(inst)                                                           \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, NULL, POST_KERNEL,                         \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_backend);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_FAKE_IPC_DEVICE)

static void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.send_result = SEND_FULL;
	memset(&session_shared, 0, sizeof(session_shared));
	memset(&test_transport, 0, sizeof(test_transport));
	test_transport.shared = &session_shared;
	test_transport.ops = &session_ops;
	test_transport.is_host = true;
	test_transport.state = MPIPE_IPC_TRANSPORT_RUNNING;
	test_transport.opened = true;
	/* Custom transports own an opaque adapter context, not necessarily the device. */
	test_transport.context = &fake;
	test_transport.session.local_sid = 11U;
	test_transport.session.remote_sid = 22U;
	test_transport.session.connected = true;
	session_shared.remote.session = MPIPE_IPC_HANDSHAKE(22U, 11U);
}

static void fake_bound(unsigned int endpoint)
{
	zassert_not_null(fake.cfg[endpoint]);
	zassert_not_null(fake.cfg[endpoint]->cb.bound);
	fake.cfg[endpoint]->cb.bound(fake.cfg[endpoint]->priv);
}

/* Deliver arbitrary bytes, which is the only thing a malformed peer can do. */
static void fake_receive_raw(unsigned int endpoint, const void *frame, size_t len)
{
	zassert_not_null(fake.cfg[endpoint]);
	zassert_not_null(fake.cfg[endpoint]->cb.received);
	fake.cfg[endpoint]->cb.received(frame, len, fake.cfg[endpoint]->priv);
}

/* Deliver a message the way a well-behaved peer would send it. */
static void fake_receive(unsigned int endpoint, const struct mpipe_ipc_msg *msg)
{
	uint8_t frame[MPIPE_IPC_WIRE_MAX_LEN];
	size_t written;

	zassert_ok(mpipe_ipc_msg_encode(frame, sizeof(frame), msg, &written));
	fake_receive_raw(endpoint, frame, written);
}

static int consume_chain(struct mpipe_pad *pad, struct net_buf *in_buf,
			 struct net_buf **out_buf)
{
	ARG_UNUSED(pad);

	if (block_consume) {
		k_sem_give(&consume_entered);
		(void)k_sem_take(&consume_release, K_FOREVER);
	}
	chain_received++;
	*out_buf = NULL;
	net_buf_unref(in_buf);
	return 0;
}

static void receive_entry(void *cfg_arg, void *msg_arg, void *unused)
{
	const struct ipc_ept_cfg *cfg = cfg_arg;
	uint8_t frame[MPIPE_IPC_WIRE_MAX_LEN];
	size_t written;

	ARG_UNUSED(unused);
	zassert_ok(mpipe_ipc_msg_encode(frame, sizeof(frame), msg_arg, &written));
	cfg->cb.received(frame, written, cfg->priv);
}

static void deinit_entry(void *src_arg, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	async_deinit_result = mpipe_ipc_src_deinit(src_arg);
	k_sem_give(&async_deinit_done);
}

static void sink_deinit_entry(void *sink_arg, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	async_sink_deinit_result = mpipe_ipc_sink_deinit(sink_arg);
	k_sem_give(&async_sink_deinit_done);
}

static int blocking_buffer_release(struct mpipe_buffer_pool *pool, struct net_buf *buf)
{
	ARG_UNUSED(pool);
	ARG_UNUSED(buf);
	k_sem_give(&buffer_release_entered);
	(void)k_sem_take(&buffer_release_continue, K_FOREVER);
	return 0;
}

static struct mpipe_buffer_pool blocking_pool = {
	.release_buffer = blocking_buffer_release,
};

static void configure_source_pipeline(struct mpipe *pipeline, struct mpipe_ipc_src *source,
				      struct mpipe_sink *sink)
{
	const struct device *ipc = DEVICE_DT_GET(FAKE_IPC_NODE);
	struct mpipe_structure caps;
	uint8_t first_id = next_element_id;
	int ret;

	next_element_id += 3U;
	memset(pipeline, 0, sizeof(*pipeline));
	memset(source, 0, sizeof(*source));
	memset(sink, 0, sizeof(*sink));

	zassert_ok(mpipe_pipeline_init(pipeline, first_id));
	ret = mpipe_ipc_src_init(source, first_id + 1U, ipc, "audio", &test_region,
				 &test_transport);
	zassert_ok(ret);
	zassert_ok(mpipe_sink_init(sink, first_id + 2U));
	sink->sink_pad.chain_fn = consume_chain;

	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					  MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT, 10000,
					  MPIPE_CAPS_NUM_OF_CHANNEL, MPIPE_TYPE_UINT, 1,
					  MPIPE_CAPS_SAMPLE_RATE, MPIPE_TYPE_UINT, 16000,
					  MPIPE_CAPS_BITWIDTH, MPIPE_TYPE_UINT, 16,
					  MPIPE_CAPS_END);
	zassert_ok(ret);
	zassert_ok(mpipe_ipc_src_set_format(source, &caps));
	zassert_ok(mpipe_bin_add(&pipeline->bin, (struct mpipe_element *)source,
				 (struct mpipe_element *)sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)source,
				      (struct mpipe_element *)sink, NULL));
	fake_bound(fake.register_count - 1U);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_reset();
	chain_received = 0U;
	block_consume = false;
	async_deinit_result = -EINPROGRESS;
	async_sink_deinit_result = -EINPROGRESS;
	k_sem_reset(&consume_entered);
	k_sem_reset(&consume_release);
	k_sem_reset(&async_deinit_done);
	k_sem_reset(&async_sink_deinit_done);
	k_sem_reset(&send_entered);
	k_sem_reset(&send_release);
	k_sem_reset(&buffer_release_entered);
	k_sem_reset(&buffer_release_continue);
}

ZTEST_SUITE(mpipe_ipc_plugin, NULL, NULL, before, NULL, NULL);

ZTEST(mpipe_ipc_plugin, test_data_follows_push_source_lifecycle)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0U,
			.size = sizeof(uint32_t),
			.buffer_id = 0U,
			.generation = 22U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);

	fake_receive(0U, &msg);
	zassert_equal(chain_received, 1U, "PLAYING source rejected callback-delivered data");

	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PAUSED),
		      0);
	fake_receive(0U, &msg);
	zassert_equal(chain_received, 1U, "PAUSED source delivered callback data");

	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_failed_push_releases_wrapper_once)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0U,
			.size = sizeof(uint32_t),
			.buffer_id = 7U,
			.generation = 22U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);
	source.base.src_pad.peer = NULL;
	fake_receive(0U, &msg);

	zassert_equal(fake.send_count, 1U, "failed push returned the same wrapper twice");
	zassert_equal(fake.sent[0].type, MPIPE_IPC_MSG_DATA_RELEASE);
	zassert_equal(fake.sent[0].release.buffer_id, 7U);
	zassert_equal(fake.sent[0].release.generation, 22U);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_out_of_range_data_is_rejected_before_push)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	static const struct {
		uint32_t offset;
		uint32_t size;
	} invalid[] = {
		{ .offset = 0U, .size = 0U },
		{ .offset = 1U, .size = sizeof(uint32_t) },
		{ .offset = 12U, .size = 8U },
		{ .offset = UINT32_MAX - 1U, .size = 8U },
	};
	struct mpipe_ipc_msg valid = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0U,
			.size = sizeof(uint32_t),
			.buffer_id = 10U,
			.generation = 22U,
		},
	};
	uint8_t wrong_version[MPIPE_IPC_WIRE_MAX_LEN];
	size_t wrong_version_len;

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);
	for (unsigned int i = 0; i < ARRAY_SIZE(invalid); i++) {
		struct mpipe_ipc_msg msg = {
			.type = MPIPE_IPC_MSG_DATA_BUFFER,
			.data = {
				.offset = invalid[i].offset,
				.size = invalid[i].size,
				.buffer_id = 5U + i,
				.generation = 22U,
			},
		};

		fake_receive(0U, &msg);
	}
	/*
	 * Otherwise well-formed, but from a peer speaking a different version.
	 * Its slot is deliberately not returned: nothing in a message this core
	 * cannot parse says which slot, or whose, it would be returning.
	 */
	zassert_ok(mpipe_ipc_msg_encode(wrong_version, sizeof(wrong_version), &valid,
					&wrong_version_len));
	wrong_version[0] = MPIPE_IPC_PLUGIN_VERSION + 1U;
	fake_receive_raw(0U, wrong_version, wrong_version_len);

	zassert_equal(chain_received, 0U, "out-of-range peer address reached downstream");
	zassert_equal(fake.send_count, ARRAY_SIZE(invalid),
		      "rejected descriptors did not return their slots");
	for (unsigned int i = 0; i < ARRAY_SIZE(invalid); i++) {
		zassert_equal(fake.sent[i].release.buffer_id, 5U + i);
	}
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_short_release_for_id_63_is_retried)
{
	const struct device *ipc = DEVICE_DT_GET(FAKE_IPC_NODE);
	struct mpipe_ipc_src source;
	struct mpipe_ipc_msg data = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .buffer_id = 63U, .generation = 22U },
	};
	int ret;

	ret = mpipe_ipc_src_init(&source, next_element_id++, ipc, "audio", &test_region,
				 &test_transport);
	zassert_ok(ret);
	fake_bound(0U);
	/* One byte short of any message: accepted by the backend, not sent. */
	fake.send_result = 1;
	fake_receive(0U, &data);
	zassert_equal(atomic_get(&source.deferred), 1U, "short release was accepted");

	fake.send_result = SEND_FULL;
	zassert_true(WAIT_FOR(fake.send_count >= 2U, 100000, k_msleep(1)),
		     "release was not retried while the peer was quiet");
	zassert_equal(fake.send_count, 2U, "buffer 63 release was not retried");
	zassert_equal(fake.sent[1].release.buffer_id, 63U);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

NET_BUF_POOL_FIXED_DEFINE(test_buffers, 1, 1, sizeof(struct mpipe_buffer_meta),
			  mpipe_buffer_destroy);

ZTEST(mpipe_ipc_plugin, test_short_data_send_is_an_error)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *out_buf;
	struct mpipe_ipc_region region;
	struct mpipe_ipc_msg release = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 11U },
	};
	int ret;

	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	region = (struct mpipe_ipc_region){
		.base = buf->data,
		.size = 1U,
		.align = 1U,
	};
	ret = mpipe_ipc_sink_init(&sink, next_element_id++, DEVICE_DT_GET(FAKE_IPC_NODE),
				  "audio", &region, &test_transport);
	zassert_ok(ret);
	fake_bound(0U);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	/* One byte short of any message: accepted by the backend, not sent. */
	fake.send_result = 1;

	ret = sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf);
	if (sink.pending[0] != NULL) {
		fake_receive(0U, &release);
	}

	zassert_equal(ret, -EMSGSIZE, "short DATA send was accepted");
	zassert_ok(mpipe_ipc_sink_deinit(&sink));
}

ZTEST(mpipe_ipc_plugin, test_stale_session_data_is_rejected_before_delivery)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0U,
			.size = sizeof(uint32_t),
			.buffer_id = 3U,
			.generation = 22U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);
	session_shared.remote.session = MPIPE_IPC_HANDSHAKE(23U, 0U);

	fake_receive(0U, &msg);

	zassert_equal(chain_received, 0U);
	zassert_equal(fake.send_count, 0U, "stale DATA must not produce a RELEASE");
	zassert_equal(test_transport.state, MPIPE_IPC_TRANSPORT_FAULTED);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_stale_data_generation_is_rejected)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0U,
			.size = sizeof(uint32_t),
			.buffer_id = 3U,
			.generation = 21U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);
	fake_receive(0U, &msg);
	zassert_equal(chain_received, 0U);
	zassert_equal(fake.send_count, 0U, "stale DATA must not produce a RELEASE");
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_release_generation_must_match_outstanding_owner)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *out_buf;
	struct mpipe_ipc_region region;
	struct mpipe_ipc_msg stale = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 10U },
	};
	struct mpipe_ipc_msg current = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 11U },
	};

	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	region = (struct mpipe_ipc_region){ .base = buf->data, .size = 1U, .align = 1U };
	zassert_ok(mpipe_ipc_sink_init(&sink, next_element_id++,
				       DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &region,
				       &test_transport));
	fake_bound(0U);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	zassert_ok(sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf));
	zassert_not_null(sink.pending[0]);
	zassert_equal(fake.sent[0].data.generation, 11U);

	fake_receive(0U, &stale);
	zassert_not_null(sink.pending[0], "old generation released a reused slot");
	fake_receive(0U, &current);
	zassert_is_null(sink.pending[0]);
	zassert_ok(mpipe_ipc_sink_deinit(&sink));
}

ZTEST(mpipe_ipc_plugin, test_two_sources_return_to_their_own_endpoint)
{
	struct mpipe first_pipe, second_pipe;
	struct mpipe_ipc_src first_src, second_src;
	struct mpipe_sink first_sink, second_sink;
	struct mpipe_ipc_msg first = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 0U, .size = 4U, .buffer_id = 1U, .generation = 22U },
	};
	struct mpipe_ipc_msg second = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 4U, .size = 4U, .buffer_id = 2U, .generation = 22U },
	};

	configure_source_pipeline(&first_pipe, &first_src, &first_sink);
	configure_source_pipeline(&second_pipe, &second_src, &second_sink);
	zassert_equal(mpipe_element_set_state(&first_pipe.bin.element, MPIPE_STATE_PLAYING),
		      0);
	zassert_equal(mpipe_element_set_state(&second_pipe.bin.element, MPIPE_STATE_PLAYING),
		      0);

	fake_receive(0U, &first);
	fake_receive(1U, &second);

	zassert_equal(fake.send_count, 2U);
	zassert_equal(fake.sent_endpoint[0], 0U);
	zassert_equal(fake.sent_endpoint[1], 1U);
	zassert_equal(mpipe_element_set_state(&first_pipe.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_equal(mpipe_element_set_state(&second_pipe.bin.element, MPIPE_STATE_READY),
		      0);
	zassert_ok(mpipe_ipc_src_deinit(&first_src));
	zassert_ok(mpipe_ipc_src_deinit(&second_src));
}

ZTEST(mpipe_ipc_plugin, test_sink_deinit_quarantines_outstanding_buffer)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *probe;
	struct net_buf *out_buf;
	struct mpipe_ipc_region region;

	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	region = (struct mpipe_ipc_region){ .base = buf->data, .size = 1U, .align = 1U };
	zassert_ok(mpipe_ipc_sink_init(&sink, next_element_id++,
				       DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &region,
				       &test_transport));
	fake_bound(0U);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	zassert_ok(sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf));
	zassert_not_null(sink.pending[0]);

	zassert_ok(mpipe_ipc_sink_deinit(&sink));
	probe = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_is_null(probe, "teardown reused memory that the failed peer may still read");

	/* Test-only reset boundary: release the quarantined reference for later cases. */
	net_buf_unref(sink.pending[0]);
	sink.pending[0] = NULL;
	probe = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(probe);
	net_buf_unref(probe);
}

ZTEST(mpipe_ipc_plugin, test_deinit_deregisters_and_closes_callback_admission)
{
	const struct ipc_ept_cfg *old_cfg;
	struct mpipe_ipc_src source;
	struct mpipe_ipc_msg data = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 0U, .size = 4U, .buffer_id = 1U, .generation = 22U },
	};

	zassert_ok(mpipe_ipc_src_init(&source, next_element_id++,
				      DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
				      &test_transport));
	old_cfg = fake.cfg[0];
	zassert_ok(mpipe_ipc_src_deinit(&source));
	zassert_equal(fake.deregister_count, 1U);
	zassert_ok(mpipe_ipc_src_deinit(&source));
	zassert_equal(fake.deregister_count, 1U, "deinit was not idempotent");

	old_cfg->cb.received(&data, sizeof(data), old_cfg->priv);
	zassert_equal(fake.send_count, 0U, "callback ran after endpoint teardown");
}

ZTEST(mpipe_ipc_plugin, test_deinit_waits_for_admitted_callback)
{
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg data = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 0U, .size = 4U, .buffer_id = 1U, .generation = 22U },
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      0);
	block_consume = true;
	(void)k_thread_create(&receive_thread, receive_stack,
			      K_THREAD_STACK_SIZEOF(receive_stack), receive_entry,
			      (void *)fake.cfg[0], &data, NULL, 0, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&consume_entered, K_SECONDS(1)));

	(void)k_thread_create(&deinit_thread, deinit_stack,
			      K_THREAD_STACK_SIZEOF(deinit_stack), deinit_entry, &source,
			      NULL, NULL, 0, 0, K_NO_WAIT);
	k_msleep(10);
	zassert_equal(fake.deregister_count, 0U,
		      "endpoint was removed while its callback was still running");
	zassert_equal(k_sem_take(&async_deinit_done, K_NO_WAIT), -EBUSY);

	k_sem_give(&consume_release);
	zassert_ok(k_thread_join(&receive_thread, K_SECONDS(1)));
	zassert_ok(k_thread_join(&deinit_thread, K_SECONDS(1)));
	zassert_ok(async_deinit_result);
	zassert_equal(fake.deregister_count, 1U);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      0);
}

ZTEST(mpipe_ipc_plugin, test_release_retry_record_is_claimed_by_one_sender)
{
	const struct device *ipc = DEVICE_DT_GET(FAKE_IPC_NODE);
	struct mpipe_ipc_src source;
	struct mpipe_ipc_msg invalid_data = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 0U, .size = 0U, .buffer_id = 9U, .generation = 22U },
	};
	struct mpipe_ipc_msg caps = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = { .media_type_id = MPIPE_MEDIA_AUDIO_PCM },
	};

	zassert_ok(mpipe_ipc_src_init(&source, next_element_id++, ipc, "audio", &test_region,
				      &test_transport));
	fake_bound(0U);
	fake.send_result = -ENOMEM;
	fake_receive(0U, &invalid_data);
	zassert_equal(fake.send_count, 1U);

	/* The retry worker owns the queued RELEASE while its send is blocked. */
	fake.send_result = SEND_FULL;
	fake.block_send_at = 2U;
	zassert_ok(k_sem_take(&send_entered, K_SECONDS(1)));
	fake_receive(0U, &caps);
	zassert_equal(fake.send_count, 2U,
		      "a callback transmitted the retry worker's claimed RELEASE again");

	k_sem_give(&send_release);
	zassert_true(WAIT_FOR(!atomic_test_bit(source.unreleased, 9U), 100000, k_msleep(1)));
	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_sink_deinit_waits_for_admitted_release_callback)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *out_buf;
	struct mpipe_ipc_region region;
	struct mpipe_ipc_msg release = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 11U },
	};

	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	region = (struct mpipe_ipc_region){ .base = buf->data, .size = 1U, .align = 1U };
	zassert_ok(mpipe_ipc_sink_init(&sink, next_element_id++,
				       DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &region,
				       &test_transport));
	fake_bound(0U);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	meta->pool = &blocking_pool;
	zassert_ok(sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf));

	(void)k_thread_create(&receive_thread, receive_stack,
			      K_THREAD_STACK_SIZEOF(receive_stack), receive_entry,
			      (void *)fake.cfg[0], &release, NULL, 0, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&buffer_release_entered, K_SECONDS(1)));
	(void)k_thread_create(&sink_deinit_thread, sink_deinit_stack,
			      K_THREAD_STACK_SIZEOF(sink_deinit_stack), sink_deinit_entry,
			      &sink, NULL, NULL, 0, 0, K_NO_WAIT);
	k_msleep(10);
	zassert_equal(fake.deregister_count, 0U);
	zassert_equal(k_sem_take(&async_sink_deinit_done, K_NO_WAIT), -EBUSY);

	k_sem_give(&buffer_release_continue);
	zassert_ok(k_thread_join(&receive_thread, K_SECONDS(1)));
	zassert_ok(k_thread_join(&sink_deinit_thread, K_SECONDS(1)));
	zassert_ok(async_sink_deinit_result);
	zassert_equal(fake.deregister_count, 1U);
	zassert_ok(mpipe_ipc_sink_deinit(&sink));
	zassert_equal(fake.deregister_count, 1U, "sink deinit was not idempotent");
}

ZTEST(mpipe_ipc_plugin, test_stale_transport_release_is_rejected)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *out_buf;
	struct mpipe_ipc_region region;
	struct mpipe_ipc_msg release = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 11U },
	};

	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	region = (struct mpipe_ipc_region){ .base = buf->data, .size = 1U, .align = 1U };
	zassert_ok(mpipe_ipc_sink_init(&sink, next_element_id++,
				       DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &region,
				       &test_transport));
	fake_bound(0U);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	zassert_ok(sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf));
	zassert_not_null(sink.pending[0]);

	session_shared.remote.session = MPIPE_IPC_HANDSHAKE(23U, 11U);
	fake_receive(0U, &release);
	zassert_not_null(sink.pending[0], "a stale transport session released storage");
	zassert_equal(test_transport.state, MPIPE_IPC_TRANSPORT_FAULTED);
	zassert_ok(mpipe_ipc_sink_deinit(&sink));

	/* Test-only paired-reset boundary. */
	net_buf_unref(sink.pending[0]);
	sink.pending[0] = NULL;
}

ZTEST(mpipe_ipc_plugin, test_deregister_failure_can_be_retried)
{
	struct mpipe_ipc_src source;

	zassert_ok(mpipe_ipc_src_init(&source, next_element_id++,
				      DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
				      &test_transport));
	fake.deregister_result = -EIO;
	zassert_equal(mpipe_ipc_src_deinit(&source), -EIO);
	zassert_equal(fake.deregister_count, 1U);

	fake.deregister_result = 0;
	zassert_ok(mpipe_ipc_src_deinit(&source));
	zassert_equal(fake.deregister_count, 2U);
}

/*
 * The encoder cannot produce a malformed CAPS, so this builds the frame by
 * hand -- which is the only way a peer can send one, and the reason the
 * decoder rather than the sender is what the source relies on. Every rejection
 * rule is covered against the codec directly in the ipc_protocol suite; what is
 * checked here is that a rejected message never becomes this element's format.
 */
static size_t build_caps_frame(uint8_t *frame, uint8_t media_type, uint8_t flags,
			       uint8_t num_fields, const uint8_t *ids,
			       const uint32_t *scalars)
{
	memset(frame, 0, MPIPE_IPC_WIRE_MAX_LEN);
	frame[0] = MPIPE_IPC_PLUGIN_VERSION;
	frame[1] = MPIPE_IPC_MSG_CAPS;
	frame[4] = media_type;
	frame[5] = flags;
	frame[6] = num_fields;

	for (uint8_t i = 0; i < num_fields; i++) {
		uint8_t *field = &frame[8U + ((size_t)i * MPIPE_IPC_WIRE_FIELD_LEN)];

		field[0] = ids[i];
		field[1] = MPIPE_TYPE_UINT;
		sys_put_le32(scalars[i], &field[4]);
	}

	return 8U + ((size_t)num_fields * MPIPE_IPC_WIRE_FIELD_LEN);
}

ZTEST(mpipe_ipc_plugin, test_malformed_caps_never_become_the_format)
{
	static const uint8_t twice[] = { MPIPE_CAPS_SAMPLE_RATE, MPIPE_CAPS_SAMPLE_RATE };
	static const uint8_t video_field[] = { MPIPE_CAPS_IMAGE_WIDTH };
	static const uint32_t rates[] = { 16000U, 16000U };
	static const uint32_t width[] = { 320U };
	struct mpipe_ipc_src source;
	uint8_t frame[MPIPE_IPC_WIRE_MAX_LEN];
	size_t len;

	zassert_ok(mpipe_ipc_src_init(&source, next_element_id++,
				      DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
				      &test_transport));

	/* One field, twice: a structure that does not name a single format. */
	len = build_caps_frame(frame, MPIPE_MEDIA_AUDIO_PCM, 0U, 2U, twice, rates);
	fake_receive_raw(0U, frame, len);
	zassert_equal(atomic_get(&source.have_caps), 0, "a duplicated field was accepted");

	/* A field belonging to another medium entirely. */
	len = build_caps_frame(frame, MPIPE_MEDIA_AUDIO_PCM, 0U, 1U, video_field, width);
	fake_receive_raw(0U, frame, len);
	zassert_equal(atomic_get(&source.have_caps), 0, "a video field was accepted as audio");

	/* A media type this build does not have. */
	len = build_caps_frame(frame, MPIPE_MEDIA_END, 0U, 0U, NULL, NULL);
	fake_receive_raw(0U, frame, len);
	zassert_equal(atomic_get(&source.have_caps), 0, "an unknown media type was accepted");

	/* Truncated: the frame claims a field it does not carry. */
	len = build_caps_frame(frame, MPIPE_MEDIA_AUDIO_PCM, 0U, 1U, twice, rates);
	fake_receive_raw(0U, frame, len - 1U);
	zassert_equal(atomic_get(&source.have_caps), 0, "a truncated CAPS was accepted");

	/* And the well-formed one it was built from still is. */
	fake_receive_raw(0U, frame, len);
	zassert_equal(atomic_get(&source.have_caps), 1, "a valid CAPS was rejected");

	zassert_ok(mpipe_ipc_src_deinit(&source));
}

ZTEST(mpipe_ipc_plugin, test_sink_proposes_its_outstanding_buffer_capacity)
{
	struct mpipe_ipc_sink sink;
	struct mpipe_dispatch query = { .type = MPIPE_DISPATCH_BUFFER_POOL };

	zassert_ok(mpipe_ipc_sink_init(&sink, next_element_id++,
				       DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
				       &test_transport));
	zassert_not_null(sink.base.propose_buffer_pool);
	zassert_ok(sink.base.propose_buffer_pool(&sink.base, &query));
	zassert_equal(query.pool_cfg.min_buffers, CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS);
	zassert_equal(query.pool_cfg.max_buffers, 0U);
	zassert_ok(mpipe_ipc_sink_deinit(&sink));
}

ZTEST(mpipe_ipc_plugin, test_init_rejects_a_peer_restart_visible_in_shared_state)
{
	struct mpipe_ipc_src source;

	session_shared.remote.session = MPIPE_IPC_HANDSHAKE(23U, 11U);
	zassert_equal(mpipe_ipc_src_init(&source, next_element_id++,
					 DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
					 &test_transport),
		      -ECONNRESET);
	zassert_equal(fake.register_count, 0U);
	zassert_equal(test_transport.state, MPIPE_IPC_TRANSPORT_FAULTED);
}

ZTEST(mpipe_ipc_plugin, test_default_transport_must_name_the_same_ipc_instance)
{
	struct mpipe_ipc_src source;

	test_transport.ops = &mpipe_ipc_zephyr_ops;
	test_transport.context = &fake;
	zassert_equal(mpipe_ipc_src_init(&source, next_element_id++,
					 DEVICE_DT_GET(FAKE_IPC_NODE), "audio", &test_region,
					 &test_transport),
		      -EINVAL);
	zassert_equal(fake.register_count, 0U);
}
