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
#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_pipeline.h>
#include <zephyr/ztest.h>

#define DT_DRV_COMPAT zephyr_mpipe_ipc_test
#define FAKE_IPC_NODE DT_NODELABEL(fake_ipc)
#define SEND_FULL INT_MIN

struct fake_ipc_context {
	const struct ipc_ept_cfg *cfg;
	struct mpipe_ipc_msg sent[8];
	size_t sent_len[8];
	unsigned int send_count;
	int send_result;
};

static struct fake_ipc_context fake;
static unsigned int chain_received;
static uint8_t next_element_id = 1U;

static int fake_register_endpoint(const struct device *instance, void **token,
				  const struct ipc_ept_cfg *cfg)
{
	ARG_UNUSED(instance);
	ARG_UNUSED(token);

	fake.cfg = cfg;
	return 0;
}

static int fake_deregister_endpoint(const struct device *instance, void *token)
{
	ARG_UNUSED(instance);
	ARG_UNUSED(token);
	return 0;
}

static int fake_send(const struct device *instance, void *token, const void *data, size_t len)
{
	unsigned int slot = fake.send_count++;

	ARG_UNUSED(instance);
	ARG_UNUSED(token);

	if (slot < ARRAY_SIZE(fake.sent)) {
		memcpy(&fake.sent[slot], data, MIN(len, sizeof(fake.sent[slot])));
		fake.sent_len[slot] = len;
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
}

static void fake_bound(void)
{
	zassert_not_null(fake.cfg);
	zassert_not_null(fake.cfg->cb.bound);
	fake.cfg->cb.bound(fake.cfg->priv);
}

static void fake_receive(const struct mpipe_ipc_msg *msg)
{
	zassert_not_null(fake.cfg);
	zassert_not_null(fake.cfg->cb.received);
	fake.cfg->cb.received(msg, sizeof(*msg), fake.cfg->priv);
}

static int consume_chain(struct mpipe_pad *pad, struct net_buf *in_buf,
			 struct net_buf **out_buf)
{
	ARG_UNUSED(pad);

	chain_received++;
	*out_buf = NULL;
	net_buf_unref(in_buf);
	return 0;
}

static void configure_source_pipeline(struct mpipe *pipeline, struct mpipe_ipc_src *source,
				      struct mpipe_sink *sink)
{
	struct mpipe_structure caps;
	uint8_t first_id = next_element_id;
	int ret;

	next_element_id += 3U;
	memset(pipeline, 0, sizeof(*pipeline));
	memset(source, 0, sizeof(*source));
	memset(sink, 0, sizeof(*sink));

	zassert_ok(mpipe_pipeline_init(pipeline, first_id));
	ret = mpipe_ipc_src_init(source, first_id + 1U, DEVICE_DT_GET(FAKE_IPC_NODE), "audio");
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
	fake_bound();
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_reset();
	chain_received = 0U;
}

ZTEST_SUITE(mpipe_ipc_plugin, NULL, NULL, before, NULL, NULL);

ZTEST(mpipe_ipc_plugin, test_data_follows_push_source_lifecycle)
{
	static uint32_t payload;
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.phys_addr = (uint32_t)(uintptr_t)&payload,
			.size = sizeof(payload),
			.buffer_id = 0U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      MPIPE_STATE_CHANGE_SUCCESS);

	fake_receive(&msg);
	zassert_equal(chain_received, 1U, "PLAYING source rejected callback-delivered data");

	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PAUSED),
		      MPIPE_STATE_CHANGE_SUCCESS);
	fake_receive(&msg);
	zassert_equal(chain_received, 1U, "PAUSED source delivered callback data");

	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_READY),
		      MPIPE_STATE_CHANGE_SUCCESS);
}

ZTEST(mpipe_ipc_plugin, test_failed_push_releases_wrapper_once)
{
	static uint32_t payload;
	struct mpipe pipeline;
	struct mpipe_ipc_src source;
	struct mpipe_sink sink;
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.phys_addr = (uint32_t)(uintptr_t)&payload,
			.size = sizeof(payload),
			.buffer_id = 7U,
		},
	};

	configure_source_pipeline(&pipeline, &source, &sink);
	zassert_equal(mpipe_element_set_state(&pipeline.bin.element, MPIPE_STATE_PLAYING),
		      MPIPE_STATE_CHANGE_SUCCESS);
	source.base.src_pad.peer = NULL;
	fake_receive(&msg);

	zassert_equal(fake.send_count, 1U, "failed push returned the same wrapper twice");
	zassert_equal(fake.sent[0].type, MPIPE_IPC_MSG_DATA_RELEASE);
	zassert_equal(fake.sent[0].release.buffer_id, 7U);
}

ZTEST(mpipe_ipc_plugin, test_short_release_for_id_63_is_retried)
{
	const struct device *ipc = DEVICE_DT_GET(FAKE_IPC_NODE);
	struct mpipe_ipc_src source;
	struct mpipe_ipc_msg data = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .buffer_id = 63U },
	};
	struct mpipe_ipc_msg tick = { .type = UINT32_MAX };
	int ret;

	ret = mpipe_ipc_src_init(&source, next_element_id++, ipc, "audio");
	zassert_ok(ret);
	fake_bound();
	fake.send_result = sizeof(struct mpipe_ipc_msg) - 1;
	fake_receive(&data);
	zassert_equal(source.deferred, 1U, "short release was accepted");

	fake.send_result = SEND_FULL;
	fake_receive(&tick);
	zassert_equal(fake.send_count, 2U, "buffer 63 release was not retried");
	zassert_equal(fake.sent[1].release.buffer_id, 63U);
}

NET_BUF_POOL_FIXED_DEFINE(test_buffers, 1, 1, sizeof(struct mpipe_buffer_meta),
			  mpipe_buffer_destroy);

ZTEST(mpipe_ipc_plugin, test_short_data_send_is_an_error)
{
	struct mpipe_buffer_meta *meta;
	struct mpipe_ipc_sink sink;
	struct net_buf *buf;
	struct net_buf *out_buf;
	struct mpipe_ipc_msg release = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U },
	};
	int ret;

	ret = mpipe_ipc_sink_init(&sink, next_element_id++, DEVICE_DT_GET(FAKE_IPC_NODE),
				  "audio");
	zassert_ok(ret);
	fake_bound();
	buf = net_buf_alloc(&test_buffers, K_NO_WAIT);
	zassert_not_null(buf);
	net_buf_add_u8(buf, 0x5a);
	meta = mpipe_buffer_get_meta(buf);
	meta->bytes_used = 1U;
	fake.send_result = sizeof(struct mpipe_ipc_msg) - 1;

	ret = sink.base.sink_pad.chain_fn(&sink.base.sink_pad, buf, &out_buf);
	if (sink.pending[0] != NULL) {
		fake_receive(&release);
	}

	zassert_equal(ret, -EMSGSIZE, "short DATA send was accepted");
}
