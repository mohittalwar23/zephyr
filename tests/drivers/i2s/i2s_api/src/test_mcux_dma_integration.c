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
#include "../../../../../drivers/i2s/i2s_mcux_sai_dma.h"

#define FAKE_DMA_CHANNELS 8U

struct fake_dma_data {
	struct dma_context context;
	atomic_t channels[ATOMIC_BITMAP_SIZE(FAKE_DMA_CHANNELS)];
	uint32_t request;
	uint32_t filter_calls;
};

static bool fake_dma_filter(const struct device *dev, int channel, void *filter_param)
{
	struct fake_dma_data *data = dev->data;

	data->request = *(uint32_t *)filter_param;
	data->filter_calls++;

	return channel != 0;
}

static int fake_dma_init(const struct device *dev)
{
	struct fake_dma_data *data = dev->data;

	data->context.magic = DMA_MAGIC;
	data->context.dma_channels = FAKE_DMA_CHANNELS;
	data->context.atomic = data->channels;

	return 0;
}

static DEVICE_API(dma, fake_dma_api) = {
	.chan_filter = fake_dma_filter,
};

static struct fake_dma_data fake_dma_data;

DEVICE_DEFINE(fake_dma, "fake_dma", fake_dma_init, NULL, &fake_dma_data, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_dma_api);

static void reset_fake_dma(void)
{
	memset(fake_dma_data.channels, 0, sizeof(fake_dma_data.channels));
	fake_dma_data.request = 0U;
	fake_dma_data.filter_calls = 0U;
}

ZTEST(mcux_dma_integration, test_sdma_request_is_independent_of_physical_channel)
{
	struct i2s_mcux_sai_dma_channel spec = {
		.channel = 6U,
		.request = 5U,
		.request_channel = true,
	};

	reset_fake_dma();
	zassert_ok(i2s_mcux_sai_dma_acquire_channel(DEVICE_GET(fake_dma), &spec));
	zassert_equal(spec.channel, 1U);
	zassert_not_equal(spec.channel, spec.request);
	zassert_equal(fake_dma_data.request, 5U);
}

ZTEST(mcux_dma_integration, test_fixed_edma_channel_does_not_request_another_channel)
{
	struct i2s_mcux_sai_dma_channel spec = {
		.channel = 6U,
		.request = 5U,
		.request_channel = false,
	};

	reset_fake_dma();
	zassert_ok(i2s_mcux_sai_dma_acquire_channel(DEVICE_GET(fake_dma), &spec));
	zassert_equal(spec.channel, 6U);
	zassert_equal(fake_dma_data.filter_calls, 0U);
}

ZTEST(mcux_dma_integration, test_sdma_widths_use_mcux_command_encoding)
{
	static const struct {
		uint32_t bytes;
		uint32_t encoded;
	} cases[] = {
		{1U, 1U},
		{2U, 2U},
		{4U, 0U},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		uint32_t encoded = UINT32_MAX;

		zassert_ok(dma_nxp_sdma_encode_width(cases[i].bytes, &encoded));
		zassert_equal(encoded, cases[i].encoded, "width %u", cases[i].bytes);
	}
}

ZTEST(mcux_dma_integration, test_sdma_rejects_unsupported_width)
{
	uint32_t encoded = UINT32_MAX;

	zassert_equal(dma_nxp_sdma_encode_width(3U, &encoded), -EINVAL);
}

ZTEST(mcux_dma_integration, test_sdma_append_mode_is_explicit)
{
	struct dma_block_config block = {
		.source_gather_en = 1U,
		.dest_scatter_en = 1U,
	};
	struct dma_config config = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.dma_slot = DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP,
		.cyclic = 1U,
		.head_block = &block,
	};
	uint32_t peripheral;
	bool append;

	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_equal(peripheral, DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP);
	zassert_false(append);

	block.source_reload_en = 1U;
	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_false(append);

	block.dest_reload_en = 1U;
	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_false(append);

	config.dma_slot |= DMA_NXP_SDMA_MODE_APPEND;
	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_true(append);
}

ZTEST(mcux_dma_integration, test_sdma_request_rejects_boundaries_without_consuming_state)
{
	struct dma_nxp_sdma_request_state request = {0};
	const uint32_t channel_count = 32U;
	const uint32_t event_count = 48U;
	uint32_t event = event_count;

	zassert_false(dma_nxp_sdma_request_admit(&request, 1, &event, channel_count,
						 event_count));
	zassert_false(request.requested);
	zassert_equal(request.event_source, 0U);

	event = event_count - 1U;
	zassert_false(dma_nxp_sdma_request_admit(&request, -1, &event, channel_count,
						 event_count));
	zassert_false(request.requested);
	zassert_true(dma_nxp_sdma_request_admit(&request, 1, &event, channel_count,
						event_count));
	zassert_true(request.requested);
	zassert_equal(request.channel, 1U);
	zassert_equal(request.event_source, 47U);

	dma_nxp_sdma_request_release(&request);
	zassert_false(request.requested);
	event = 4U;
	zassert_true(dma_nxp_sdma_request_admit(&request, 2, &event, channel_count,
						event_count));
	zassert_equal(request.channel, 2U, "released request state was not reusable");
}

ZTEST(mcux_dma_integration, test_sdma_context_storage_is_per_controller)
{
	struct test_context {
		uint32_t words[4];
	};
	struct test_context controller_a[4] = {0};
	struct test_context controller_b[4] = {0};
	struct dma_nxp_sdma_context_store store_a = {
		.base = controller_a,
		.stride = sizeof(controller_a[0]),
		.count = ARRAY_SIZE(controller_a),
	};
	struct dma_nxp_sdma_context_store store_b = {
		.base = controller_b,
		.stride = sizeof(controller_b[0]),
		.count = ARRAY_SIZE(controller_b),
	};
	struct test_context *context_a = dma_nxp_sdma_context_at(&store_a, 2U);
	struct test_context *context_b = dma_nxp_sdma_context_at(&store_b, 2U);

	zassert_not_null(context_a);
	zassert_not_null(context_b);
	zassert_not_equal(context_a, context_b);
	context_a->words[0] = 0xaaaaaaaaU;
	context_b->words[0] = 0x55555555U;
	zassert_equal(controller_a[2].words[0], 0xaaaaaaaaU);
	zassert_equal(controller_b[2].words[0], 0x55555555U);
}

ZTEST(mcux_dma_integration, test_sdma_ram_script_rejects_a_second_controller)
{
	struct dma_nxp_sdma_ram_script_state script = {0};
	bool channel_a_claimed = false;
	bool channel_b_claimed = false;
	uint32_t controller_a = 0U;
	uint32_t controller_b = 0U;

	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller_a, false,
						&channel_a_claimed));
	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller_b, false,
						&channel_b_claimed));
	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller_a, true,
						&channel_a_claimed));
	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller_a, true,
						&channel_a_claimed));
	zassert_equal(dma_nxp_sdma_ram_script_claim(&script, &controller_b, true,
						   &channel_b_claimed),
		      -ENOTSUP);
	zassert_equal(script.claim_count, 1U, "reconfiguration double-counted one channel");
}

ZTEST(mcux_dma_integration, test_sdma_ram_script_releases_only_the_final_channel)
{
	struct dma_nxp_sdma_ram_script_state script = {0};
	bool channel_one_claimed = false;
	bool channel_two_claimed = false;
	bool foreign_channel_claimed = false;
	uint32_t controller = 0U;
	uint32_t foreign_controller = 0U;

	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller, true,
						&channel_one_claimed));
	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller, true,
						&channel_two_claimed));
	zassert_equal(script.claim_count, 2U);

	dma_nxp_sdma_ram_script_release(&script, &controller, &channel_one_claimed);
	zassert_false(channel_one_claimed);
	zassert_equal(script.claim_count, 1U);
	zassert_equal(dma_nxp_sdma_ram_script_claim(&script, &foreign_controller, true,
						   &foreign_channel_claimed),
		      -ENOTSUP, "one channel releasing must not release its controller's peer");

	dma_nxp_sdma_ram_script_release(&script, &controller, &channel_two_claimed);
	zassert_false(channel_two_claimed);
	zassert_equal(script.claim_count, 0U);
	zassert_is_null(script.owner);
	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &foreign_controller, true,
						&foreign_channel_claimed));
}

ZTEST(mcux_dma_integration, test_sdma_ram_script_ignores_foreign_and_duplicate_release)
{
	struct dma_nxp_sdma_ram_script_state script = {0};
	bool claimed = false;
	uint32_t controller = 0U;
	uint32_t foreign_controller = 0U;

	zassert_ok(dma_nxp_sdma_ram_script_claim(&script, &controller, true, &claimed));
	dma_nxp_sdma_ram_script_release(&script, &foreign_controller, &claimed);
	zassert_true(claimed);
	zassert_equal(script.claim_count, 1U);

	dma_nxp_sdma_ram_script_release(&script, &controller, &claimed);
	dma_nxp_sdma_ram_script_release(&script, &controller, &claimed);
	zassert_false(claimed);
	zassert_equal(script.claim_count, 0U);
}

ZTEST(mcux_dma_integration, test_sdma_slot_validation_covers_supported_peripherals)
{
	struct dma_config config = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.cyclic = 1U,
		.dma_slot = DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP | DMA_NXP_SDMA_MODE_APPEND,
	};
	uint32_t peripheral = UINT32_MAX;
	bool append = false;

	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_equal(peripheral, DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP);
	zassert_true(append);

	config.channel_direction = PERIPHERAL_TO_MEMORY;
	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_true(append);

	config.dma_slot = DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM |
			  DMA_NXP_SDMA_MODE_APPEND;
	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_equal(peripheral, DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM);

	config.channel_direction = MEMORY_TO_PERIPHERAL;
	zassert_equal(dma_nxp_sdma_validate_slot(&config, &peripheral, &append), -EINVAL);
	config.channel_direction = PERIPHERAL_TO_MEMORY;
	config.cyclic = 0U;
	zassert_equal(dma_nxp_sdma_validate_slot(&config, &peripheral, &append), -EINVAL);
	config.cyclic = 1U;
	config.dma_slot = 7U | DMA_NXP_SDMA_MODE_APPEND;
	zassert_equal(dma_nxp_sdma_validate_slot(&config, &peripheral, &append), -EINVAL);
}

ZTEST(mcux_dma_integration, test_sdma_fixed_ring_requires_event_admission)
{
	struct dma_nxp_sdma_request_state request = {0};
	struct dma_config config = {
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.cyclic = 1U,
		.dma_slot = DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP,
	};
	uint32_t peripheral;
	uint32_t event = 4U;
	bool append = true;

	zassert_ok(dma_nxp_sdma_validate_slot(&config, &peripheral, &append));
	zassert_false(append, "fixed ring unexpectedly selected append mode");
	zassert_equal(dma_nxp_sdma_request_validate(&request, 3U), -EINVAL,
		      "fixed ring configured without event admission");
	zassert_true(dma_nxp_sdma_request_admit(&request, 3, &event, 32U, 48U));
	zassert_ok(dma_nxp_sdma_request_validate(&request, 3U),
		   "fixed ring did not latch its event before config");
	zassert_equal(request.event_source, 4U);
}

ZTEST_SUITE(mcux_dma_integration, NULL, NULL, NULL, NULL, NULL);
