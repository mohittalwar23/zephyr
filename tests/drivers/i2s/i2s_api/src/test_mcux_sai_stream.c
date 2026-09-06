/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "../../../../../drivers/dma/dma_nxp_sdma_common.h"
#include "../../../../../drivers/i2s/i2s_mcux_sai_stream.h"

#define FAKE_SAI_DMA_CHANNELS 8U
#define TEST_FIFO_ADDRESS     0x40000000U
#define TEST_BLOCK_SIZE       32U
#define TEST_BLOCK_COUNT      8U
#define TEST_QUEUE_DEPTH      4U

struct fake_sai_dma_data {
	struct dma_context context;
	atomic_t channels[ATOMIC_BITMAP_SIZE(FAKE_SAI_DMA_CHANNELS)];
	int config_status;
	int reload_status;
	int start_status;
	uint32_t grant_limit;
	uint32_t granted;
	uint32_t config_calls;
	uint32_t reload_calls;
	uint32_t start_calls;
	uint32_t stop_calls;
	uint32_t release_calls;
	uint32_t released[FAKE_SAI_DMA_CHANNELS];
	struct dma_config captured_config;
};

static struct fake_sai_dma_data fake_sai_dma_data = {
	.grant_limit = FAKE_SAI_DMA_CHANNELS,
};

static int fake_sai_dma_config(const struct device *dev, uint32_t channel,
			       struct dma_config *config)
{
	struct fake_sai_dma_data *data = dev->data;

	ARG_UNUSED(channel);

	data->captured_config = *config;
	data->config_calls++;

	return data->config_status;
}

static int fake_sai_dma_reload(const struct device *dev, uint32_t channel, uint32_t src,
			       uint32_t dst, size_t size)
{
	struct fake_sai_dma_data *data = dev->data;

	ARG_UNUSED(channel);
	ARG_UNUSED(src);
	ARG_UNUSED(dst);
	ARG_UNUSED(size);

	data->reload_calls++;

	return data->reload_status;
}

static int fake_sai_dma_start(const struct device *dev, uint32_t channel)
{
	struct fake_sai_dma_data *data = dev->data;

	ARG_UNUSED(channel);

	data->start_calls++;

	return data->start_status;
}

static int fake_sai_dma_stop(const struct device *dev, uint32_t channel)
{
	struct fake_sai_dma_data *data = dev->data;

	ARG_UNUSED(channel);

	data->stop_calls++;

	return 0;
}

static bool fake_sai_dma_filter(const struct device *dev, int channel, void *filter_param)
{
	struct fake_sai_dma_data *data = dev->data;

	ARG_UNUSED(filter_param);

	if (channel == 0 || data->granted >= data->grant_limit) {
		return false;
	}

	data->granted++;

	return true;
}

static void fake_sai_dma_release(const struct device *dev, uint32_t channel)
{
	struct fake_sai_dma_data *data = dev->data;

	if (data->release_calls < ARRAY_SIZE(data->released)) {
		data->released[data->release_calls] = channel;
	}
	data->release_calls++;
	if (data->granted > 0U) {
		data->granted--;
	}
}

static int fake_sai_dma_init(const struct device *dev)
{
	struct fake_sai_dma_data *data = dev->data;

	data->context.magic = DMA_MAGIC;
	data->context.dma_channels = FAKE_SAI_DMA_CHANNELS;
	data->context.atomic = data->channels;

	return 0;
}

static int fake_sai_dma_unready_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -EIO;
}

static DEVICE_API(dma, fake_sai_dma_api) = {
	.config = fake_sai_dma_config,
	.reload = fake_sai_dma_reload,
	.start = fake_sai_dma_start,
	.stop = fake_sai_dma_stop,
	.chan_filter = fake_sai_dma_filter,
	.chan_release = fake_sai_dma_release,
};

DEVICE_DEFINE(fake_sai_dma, "fake_sai_dma", fake_sai_dma_init, NULL, &fake_sai_dma_data, NULL,
	      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_sai_dma_api);

static struct fake_sai_dma_data fake_sai_dma_unready_data;

DEVICE_DEFINE(fake_sai_dma_unready, "fake_sai_dma_unready", fake_sai_dma_unready_init, NULL,
	      &fake_sai_dma_unready_data, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
	      &fake_sai_dma_api);

static char test_slab_buffer[TEST_BLOCK_COUNT * TEST_BLOCK_SIZE] __aligned(4);
static struct k_mem_slab test_slab;

static struct i2s_mcux_sai_stream stream;
static char test_in_msgs[TEST_QUEUE_DEPTH * sizeof(struct i2s_mcux_sai_q_entry)];
static char test_out_msgs[TEST_QUEUE_DEPTH * sizeof(struct i2s_mcux_sai_q_entry)];

static const struct device *fake_dma_dev(void)
{
	return DEVICE_GET(fake_sai_dma);
}

static void stream_setup(enum i2s_state state)
{
	memset(&stream, 0, sizeof(stream));
	k_msgq_init(&stream.in_queue, test_in_msgs, sizeof(struct i2s_mcux_sai_q_entry),
		    TEST_QUEUE_DEPTH);
	k_msgq_init(&stream.out_queue, test_out_msgs, sizeof(struct i2s_mcux_sai_q_entry),
		    TEST_QUEUE_DEPTH);
	stream.cfg.mem_slab = &test_slab;
	stream.cfg.block_size = TEST_BLOCK_SIZE;
	stream.dma.channel = 1U;
	stream.dma.request_channel = true;
	stream.max_dma_blocks = 2U;
	stream.state = state;
}

static void queue_blocks(struct k_msgq *queue, uint32_t count)
{
	for (uint32_t i = 0U; i < count; i++) {
		struct i2s_mcux_sai_q_entry q_entry = {.size = TEST_BLOCK_SIZE};

		zassert_ok(k_mem_slab_alloc(&test_slab, &q_entry.mem_block, K_NO_WAIT));
		zassert_ok(k_msgq_put(queue, &q_entry, K_NO_WAIT));
	}
}

static enum i2s_mcux_sai_stream_action reported_action;

/* Stands in for the SAI front end callback, which cannot build on the host. */
static void test_tx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
			     int status)
{
	ARG_UNUSED(channel);

	reported_action = i2s_mcux_sai_stream_tx_complete(arg, dma_dev, TEST_FIFO_ADDRESS, status);
}

static void test_rx_callback(const struct device *dma_dev, void *arg, uint32_t channel,
			     int status)
{
	ARG_UNUSED(channel);

	reported_action = i2s_mcux_sai_stream_rx_complete(arg, dma_dev, TEST_FIFO_ADDRESS, status);
}

static void fake_sai_dma_report(int status)
{
	struct dma_config *config = &fake_sai_dma_data.captured_config;

	/* the notification gate dma_nxp_sdma applies to every completion */
	if (status < 0 && config->error_callback_dis) {
		return;
	}

	config->dma_callback(fake_dma_dev(), config->user_data, 1U, status);
}

static void stream_before(void *fixture)
{
	struct dma_context context = fake_sai_dma_data.context;

	ARG_UNUSED(fixture);

	memset(&fake_sai_dma_data, 0, sizeof(fake_sai_dma_data));
	fake_sai_dma_data.context = context;
	fake_sai_dma_data.grant_limit = FAKE_SAI_DMA_CHANNELS;
	zassert_ok(k_mem_slab_init(&test_slab, test_slab_buffer, TEST_BLOCK_SIZE,
				   TEST_BLOCK_COUNT));
	reported_action = I2S_MCUX_SAI_STREAM_RUN;
	stream_setup(I2S_STATE_READY);
}

ZTEST(mcux_sai_stream, test_dma_width_matches_the_controller_encoding)
{
	uint32_t encoded = UINT32_MAX;
	uint8_t width = 0U;

	zassert_ok(i2s_mcux_sai_stream_dma_width(8U, &width));
	zassert_equal(width, 1U);
	zassert_ok(i2s_mcux_sai_stream_dma_width(16U, &width));
	zassert_equal(width, 2U);
	zassert_ok(i2s_mcux_sai_stream_dma_width(32U, &width));
	zassert_equal(width, 4U);

	zassert_ok(i2s_mcux_sai_stream_dma_width(24U, &width));
	zassert_equal(width, 4U, "24-bit samples occupy four bytes in an I2S buffer");
	zassert_ok(dma_nxp_sdma_encode_width(width, &encoded),
		   "the width handed to SDMA must be encodable");

	zassert_ok(i2s_mcux_sai_stream_dma_width(20U, &width));
	zassert_equal(width, 4U);
	zassert_ok(dma_nxp_sdma_encode_width(width, &encoded));

	zassert_equal(i2s_mcux_sai_stream_dma_width(0U, &width), -EINVAL);
	zassert_equal(i2s_mcux_sai_stream_dma_width(33U, &width), -EINVAL);
}

ZTEST(mcux_sai_stream, test_tx_start_unwinds_when_dma_config_fails)
{
	queue_blocks(&stream.in_queue, 1U);
	fake_sai_dma_data.config_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_tx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO, "a rejected channel configuration must be reported");
	zassert_equal(fake_sai_dma_data.start_calls, 0U,
		      "an unconfigured channel must not be started");
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U,
		      "no buffer may be handed to a channel that was not configured");
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U);
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT,
		      "the dequeued buffer was not released exactly once");
}

ZTEST(mcux_sai_stream, test_rx_start_unwinds_when_dma_config_fails)
{
	fake_sai_dma_data.config_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_rx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO);
	zassert_equal(fake_sai_dma_data.start_calls, 0U);
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U);
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT,
		      "the allocated buffer was not released exactly once");
}

ZTEST(mcux_sai_stream, test_rx_start_rejects_slab_without_replacement_block)
{
	void *reserved[TEST_BLOCK_COUNT - I2S_MCUX_SAI_RX_PREP_BLOCKS];
	uint32_t config_calls;
	int ret;

	for (size_t i = 0U; i < ARRAY_SIZE(reserved); i++) {
		zassert_ok(k_mem_slab_alloc(&test_slab, &reserved[i], K_NO_WAIT));
	}
	zassert_equal(k_mem_slab_num_free_get(&test_slab), I2S_MCUX_SAI_RX_PREP_BLOCKS);

	ret = i2s_mcux_sai_stream_rx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS);
	config_calls = fake_sai_dma_data.config_calls;

	if (ret == 0) {
		zassert_ok(dma_stop(fake_dma_dev(), stream.dma.channel));
		i2s_mcux_sai_stream_purge(&stream, true, false);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(reserved); i++) {
		k_mem_slab_free(&test_slab, reserved[i]);
	}

	zassert_equal(ret, -EINVAL,
		      "RX needs one free replacement in addition to its prepared DMA blocks");
	zassert_equal(config_calls, 0U, "an undersized slab must be rejected before DMA setup");
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT);
}

ZTEST(mcux_sai_stream, test_rx_start_releases_the_block_a_failed_reload_never_took)
{
	fake_sai_dma_data.reload_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_rx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO);
	zassert_equal(fake_sai_dma_data.start_calls, 0U);
	zassert_equal(fake_sai_dma_data.stop_calls, 1U);
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U,
		      "a failed start must not retain a stale DMA descriptor");
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT,
		      "every block prepared by the failed start must be released");
}

ZTEST(mcux_sai_stream, test_tx_start_unwinds_prepared_blocks_when_reload_fails)
{
	queue_blocks(&stream.in_queue, 2U);
	fake_sai_dma_data.reload_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_tx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO);
	zassert_equal(fake_sai_dma_data.start_calls, 0U);
	zassert_equal(fake_sai_dma_data.stop_calls, 1U);
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U,
		      "a failed start must not retain a stale DMA descriptor");
	zassert_equal(stream.free_tx_dma_blocks, stream.max_dma_blocks);
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT,
		      "every block consumed by the failed start must be released");
}

ZTEST(mcux_sai_stream, test_tx_start_unwinds_prepared_blocks_when_dma_start_fails)
{
	queue_blocks(&stream.in_queue, 3U);
	fake_sai_dma_data.start_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_tx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO);
	zassert_equal(fake_sai_dma_data.stop_calls, 1U);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U,
		      "a rejected DMA start must release every prepared descriptor");
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 1U,
		      "a block not submitted to DMA remains queued for a retry");
	zassert_equal(stream.free_tx_dma_blocks, stream.max_dma_blocks);
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT - 1U);
}

ZTEST(mcux_sai_stream, test_rx_start_unwinds_prepared_blocks_when_dma_start_fails)
{
	fake_sai_dma_data.start_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_rx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS),
		      -EIO);
	zassert_equal(fake_sai_dma_data.stop_calls, 1U);
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U,
		      "a rejected DMA start must release every prepared descriptor");
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT);
}

ZTEST(mcux_sai_stream, test_tx_reload_releases_the_block_a_failed_reload_never_took)
{
	uint8_t blocks_queued = UINT8_MAX;

	stream.state = I2S_STATE_RUNNING;
	stream.free_tx_dma_blocks = 2U;
	queue_blocks(&stream.in_queue, 1U);
	fake_sai_dma_data.reload_status = -EIO;

	zassert_equal(i2s_mcux_sai_stream_tx_reload(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS,
						    &blocks_queued),
		      -EIO);
	zassert_equal(blocks_queued, 0U);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U);
	zassert_equal(k_msgq_num_used_get(&stream.in_queue), 0U);
	zassert_equal(stream.free_tx_dma_blocks, 2U,
		      "a rejected reload must not consume a DMA block");
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT,
		      "the rejected buffer was leaked");
}

ZTEST(mcux_sai_stream, test_tx_complete_error_status_does_not_complete_a_block)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_RUNNING;
	stream.free_tx_dma_blocks = 1U;
	queue_blocks(&stream.out_queue, 1U);

	action = i2s_mcux_sai_stream_tx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, -EIO);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_STOP);
	zassert_equal(stream.state, I2S_STATE_ERROR);
	zassert_equal(stream.free_tx_dma_blocks, 1U,
		      "an error notification is not a completed audio block");
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 1U);
	zassert_equal(fake_sai_dma_data.reload_calls, 0U,
		      "a failed channel must not be reloaded");
}

ZTEST(mcux_sai_stream, test_rx_complete_error_status_does_not_deliver_a_block)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_RUNNING;
	queue_blocks(&stream.in_queue, 1U);

	action = i2s_mcux_sai_stream_rx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, -EIO);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_STOP);
	zassert_equal(stream.state, I2S_STATE_ERROR);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U,
		      "an error notification must not deliver an audio block");
	zassert_equal(fake_sai_dma_data.reload_calls, 0U);
}

ZTEST(mcux_sai_stream, test_rx_complete_releases_the_block_a_failed_reload_never_took)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_RUNNING;
	queue_blocks(&stream.in_queue, 1U);
	fake_sai_dma_data.reload_status = -EIO;

	action = i2s_mcux_sai_stream_rx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, 0);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_STOP);
	zassert_equal(stream.state, I2S_STATE_ERROR);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 1U,
		      "the received block must still be delivered");
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT - 1U,
		      "the rejected buffer was leaked");
}

ZTEST(mcux_sai_stream, test_tx_complete_pauses_when_the_queue_runs_dry)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_RUNNING;
	stream.free_tx_dma_blocks = 1U;
	queue_blocks(&stream.out_queue, 1U);

	action = i2s_mcux_sai_stream_tx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, 0);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_PAUSE);
	zassert_equal(stream.state, I2S_STATE_READY);
	zassert_equal(stream.free_tx_dma_blocks, 2U);
	zassert_equal(k_mem_slab_num_free_get(&test_slab), TEST_BLOCK_COUNT);
}

ZTEST(mcux_sai_stream, test_rx_complete_drains_the_in_flight_blocks_when_stopping)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_STOPPING;
	queue_blocks(&stream.in_queue, 1U);

	action = i2s_mcux_sai_stream_rx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, 0);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_STOP_DRAIN);
	zassert_equal(stream.state, I2S_STATE_READY);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 1U);
	zassert_equal(fake_sai_dma_data.reload_calls, 0U);
}

ZTEST(mcux_sai_stream, test_channel_pair_rejects_an_unready_controller)
{
	struct i2s_mcux_sai_dma_channel tx = {.request = 1U, .request_channel = true};
	struct i2s_mcux_sai_dma_channel rx = {.request = 2U, .request_channel = true};

	zassert_equal(i2s_mcux_sai_dma_acquire_pair(DEVICE_GET(fake_sai_dma_unready), &tx, &rx),
		      -ENODEV, "an unready controller must not be used");
	zassert_false(tx.acquired);
	zassert_false(rx.acquired);
	zassert_equal(i2s_mcux_sai_dma_acquire_pair(NULL, &tx, &rx), -ENODEV);
}

ZTEST(mcux_sai_stream, test_channel_pair_releases_tx_when_rx_cannot_be_acquired)
{
	struct i2s_mcux_sai_dma_channel tx = {.request = 1U, .request_channel = true};
	struct i2s_mcux_sai_dma_channel rx = {.request = 2U, .request_channel = true};

	fake_sai_dma_data.grant_limit = 1U;

	zassert_true(i2s_mcux_sai_dma_acquire_pair(fake_dma_dev(), &tx, &rx) < 0);
	zassert_false(tx.acquired, "the transmit channel was leaked");
	zassert_false(rx.acquired);
	zassert_equal(fake_sai_dma_data.release_calls, 1U);
	zassert_equal(fake_sai_dma_data.released[0], 1U);
}

ZTEST(mcux_sai_stream, test_channel_pair_is_released_in_reverse_order)
{
	struct i2s_mcux_sai_dma_channel tx = {.request = 1U, .request_channel = true};
	struct i2s_mcux_sai_dma_channel rx = {.request = 2U, .request_channel = true};

	zassert_ok(i2s_mcux_sai_dma_acquire_pair(fake_dma_dev(), &tx, &rx));
	zassert_true(tx.acquired);
	zassert_true(rx.acquired);
	zassert_not_equal(tx.channel, rx.channel);

	i2s_mcux_sai_dma_release_pair(fake_dma_dev(), &tx, &rx);

	zassert_equal(fake_sai_dma_data.release_calls, 2U);
	zassert_equal(fake_sai_dma_data.released[0], rx.channel);
	zassert_equal(fake_sai_dma_data.released[1], tx.channel);
	zassert_false(tx.acquired);
	zassert_false(rx.acquired);

	i2s_mcux_sai_dma_release_pair(fake_dma_dev(), &tx, &rx);
	zassert_equal(fake_sai_dma_data.release_calls, 2U,
		      "a channel must not be released twice");
}

ZTEST(mcux_sai_stream, test_fixed_channel_pair_is_never_released)
{
	struct i2s_mcux_sai_dma_channel tx = {.channel = 4U};
	struct i2s_mcux_sai_dma_channel rx = {.channel = 5U};

	zassert_ok(i2s_mcux_sai_dma_acquire_pair(fake_dma_dev(), &tx, &rx));
	zassert_equal(tx.channel, 4U);
	zassert_equal(rx.channel, 5U);

	i2s_mcux_sai_dma_release_pair(fake_dma_dev(), &tx, &rx);

	zassert_equal(fake_sai_dma_data.release_calls, 0U,
		      "a fixed channel is not owned by the request allocator");
}

ZTEST(mcux_sai_stream, test_fifo_channel_mask_is_preserved)
{
	zassert_equal(i2s_mcux_sai_stream_channel_mask(0x1U), 0x1U);
	zassert_equal(i2s_mcux_sai_stream_channel_mask(0x5U), 0x5U);
}

ZTEST(mcux_sai_stream, test_caps_report_directional_buffer_requirements)
{
	zassert_equal(i2s_mcux_sai_stream_min_buffers(I2S_DIR_TX), 1U);
	zassert_equal(i2s_mcux_sai_stream_min_buffers(I2S_DIR_RX),
		      I2S_MCUX_SAI_RX_PREP_BLOCKS + 1U);
}

/*
 * Linux's fsl_sai picks the burst first (FSL_SAI_MAXBURST_TX is 6 words) and
 * derives the watermark as depth - maxburst. Deriving it the other way round,
 * from a half-full watermark, asks for 64 words per request instead of 6.
 */
ZTEST(mcux_sai_stream, test_sdma_fifo_request_derives_watermark_from_a_small_burst)
{
	struct i2s_mcux_sai_tx_fifo_config config =
		i2s_mcux_sai_stream_tx_fifo_config(128U, 4U, true);

	zassert_equal(config.burst_length, 6U * 4U, "burst must stay a few words");
	zassert_equal(config.watermark, 128U - 6U, "watermark follows from the burst");
}

/* A FIFO smaller than the burst must not underflow the watermark. */
ZTEST(mcux_sai_stream, test_sdma_fifo_request_handles_a_tiny_fifo)
{
	struct i2s_mcux_sai_tx_fifo_config config =
		i2s_mcux_sai_stream_tx_fifo_config(4U, 4U, true);

	zassert_equal(config.burst_length, 4U * 4U);
	zassert_equal(config.watermark, 0U);
}

ZTEST(mcux_sai_stream, test_edma_fifo_request_preserves_single_word_burst)
{
	struct i2s_mcux_sai_tx_fifo_config config =
		i2s_mcux_sai_stream_tx_fifo_config(128U, 4U, false);

	zassert_equal(config.watermark, 127U);
	zassert_equal(config.burst_length, 4U);
}

ZTEST(mcux_sai_stream, test_a_failed_tx_transfer_reaches_the_stream)
{
	stream.dma_cfg.dma_callback = test_tx_callback;
	stream.dma_cfg.user_data = &stream;
	stream.dma_cfg.error_callback_dis = 1U;
	queue_blocks(&stream.in_queue, 1U);

	zassert_ok(i2s_mcux_sai_stream_tx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS));
	zassert_equal(fake_sai_dma_data.captured_config.error_callback_dis, 0U,
		      "the controller must not be told to drop a failed transfer");

	stream.state = I2S_STATE_RUNNING;
	fake_sai_dma_report(-EIO);

	zassert_equal(reported_action, I2S_MCUX_SAI_STREAM_STOP,
		      "the failed transfer never reached the stream");
	zassert_equal(stream.state, I2S_STATE_ERROR);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 1U);
}

ZTEST(mcux_sai_stream, test_a_failed_rx_transfer_reaches_the_stream)
{
	stream.dma_cfg.dma_callback = test_rx_callback;
	stream.dma_cfg.user_data = &stream;
	stream.dma_cfg.error_callback_dis = 1U;

	zassert_ok(i2s_mcux_sai_stream_rx_start(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS));
	zassert_equal(fake_sai_dma_data.captured_config.error_callback_dis, 0U,
		      "the controller must not be told to drop a failed transfer");

	stream.state = I2S_STATE_RUNNING;
	fake_sai_dma_report(-EIO);

	zassert_equal(reported_action, I2S_MCUX_SAI_STREAM_STOP,
		      "the failed transfer never reached the stream");
	zassert_equal(stream.state, I2S_STATE_ERROR);
	zassert_equal(k_msgq_num_used_get(&stream.out_queue), 0U);
}

ZTEST(mcux_sai_stream, test_rx_complete_reports_a_callback_it_cannot_handle)
{
	enum i2s_mcux_sai_stream_action action;

	stream.state = I2S_STATE_READY;

	action = i2s_mcux_sai_stream_rx_complete(&stream, fake_dma_dev(), TEST_FIFO_ADDRESS, 0);

	zassert_equal(action, I2S_MCUX_SAI_STREAM_IGNORE,
		      "a callback in an unhandled state must be reported");
	zassert_equal(stream.state, I2S_STATE_READY);
}

ZTEST_SUITE(mcux_sai_stream, NULL, NULL, stream_before, NULL, NULL);
