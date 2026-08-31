/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT vnd_nxp_sai_clock_consumer

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nxp_clock_control.h>
#include <zephyr/ztest.h>

#include "dma_nxp_sdma_clock.h"
#include "micfil_clock.h"
#include "sai_clock_dt.h"

#define CLOCK_A_NODE       DT_NODELABEL(clock_a)
#define CLOCK_B_NODE       DT_NODELABEL(clock_b)
#define CLOCK_UNREADY_NODE DT_NODELABEL(clock_unready)

struct fake_clock_data {
	int on_result;
	int off_result;
	int get_rate_result;
	uint32_t rate;
	uint32_t on_count;
	uint32_t off_count;
	uint32_t get_rate_count;
	uint32_t sequence;
	clock_control_subsys_t on_subsys;
	clock_control_subsys_t off_subsys;
	clock_control_subsys_t get_rate_subsys;
};

static uint32_t sequence;

static int fake_clock_on(const struct device *dev, clock_control_subsys_t subsys)
{
	struct fake_clock_data *data = dev->data;

	data->on_count++;
	data->sequence = ++sequence;
	data->on_subsys = subsys;

	return data->on_result;
}

static int fake_clock_off(const struct device *dev, clock_control_subsys_t subsys)
{
	struct fake_clock_data *data = dev->data;

	data->off_count++;
	data->off_subsys = subsys;

	return data->off_result;
}

static int fake_clock_get_rate(const struct device *dev, clock_control_subsys_t subsys,
			       uint32_t *rate)
{
	struct fake_clock_data *data = dev->data;

	data->get_rate_count++;
	data->get_rate_subsys = subsys;
	if (data->get_rate_result < 0) {
		return data->get_rate_result;
	}

	*rate = data->rate;

	return 0;
}

static int fake_clock_ready_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int fake_clock_unready_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENODEV;
}

static DEVICE_API(clock_control, fake_clock_api) = {
	.on = fake_clock_on,
	.off = fake_clock_off,
	.get_rate = fake_clock_get_rate,
};

static struct fake_clock_data clock_a_data;
static struct fake_clock_data clock_b_data;
static struct fake_clock_data clock_unready_data;

DEVICE_DT_DEFINE(CLOCK_A_NODE, fake_clock_ready_init, NULL, &clock_a_data, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);
DEVICE_DT_DEFINE(CLOCK_B_NODE, fake_clock_ready_init, NULL, &clock_b_data, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);
DEVICE_DT_DEFINE(CLOCK_UNREADY_NODE, fake_clock_unready_init, NULL, &clock_unready_data,
		 NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);

static const struct sai_clock_data sai_clock_data = SAI_CLOCK_DATA_DECLARE(0);

static void reset_clock(struct fake_clock_data *data, uint32_t rate)
{
	*data = (struct fake_clock_data){
		.rate = rate,
	};
}

static void nxp_audio_clock_before(void *fixture)
{
	ARG_UNUSED(fixture);

	sequence = 0U;
	reset_clock(&clock_a_data, 24576000U);
	reset_clock(&clock_b_data, 196608000U);
	reset_clock(&clock_unready_data, 24576000U);
}

static void fake_sdma_init(void *context)
{
	uint32_t *init_sequence = context;

	*init_sequence = ++sequence;
}

ZTEST(nxp_audio_clock, test_specs_forward_each_provider_and_literal_subsystem)
{
	const struct nxp_clock_dt_spec clocks[] = {
		{.dev = DEVICE_DT_GET(CLOCK_A_NODE), .subsys = UINT_TO_POINTER(0x33U)},
		{.dev = DEVICE_DT_GET(CLOCK_B_NODE), .subsys = UINT_TO_POINTER(0x55U)},
	};

	zassert_ok(nxp_clock_control_on_dt(&clocks[0]));
	zassert_ok(nxp_clock_control_on_dt(&clocks[1]));
	zassert_equal(clock_a_data.on_count, 1U);
	zassert_equal(clock_b_data.on_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.on_subsys), 0x33U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.on_subsys), 0x55U);
}

ZTEST(nxp_audio_clock, test_sai_dt_specs_forward_indexed_providers_and_name_cells)
{
	zassert_equal(sai_clock_data.clock_num, 2U);
	zassert_ok(nxp_clock_control_on_dt(&sai_clock_data.clocks[0]));
	zassert_ok(nxp_clock_control_on_dt(&sai_clock_data.clocks[1]));
	zassert_equal(clock_a_data.on_count, 1U);
	zassert_equal(clock_b_data.on_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.on_subsys), 0x33U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.on_subsys), 0x55U);
}

ZTEST(nxp_audio_clock, test_sdma_rejects_unready_clock_provider)
{
	const struct nxp_clock_dt_spec clock = {
		.dev = DEVICE_DT_GET(CLOCK_UNREADY_NODE),
		.subsys = UINT_TO_POINTER(0x33U),
	};
	uint32_t init_sequence = 0U;

	zassert_equal(dma_nxp_sdma_clocked_init(&clock, fake_sdma_init, &init_sequence),
		      -ENODEV);
	zassert_equal(init_sequence, 0U);
	zassert_equal(clock_unready_data.on_count, 0U);
}

ZTEST(nxp_audio_clock, test_sdma_preserves_clock_backend_error)
{
	const struct nxp_clock_dt_spec clock = {
		.dev = DEVICE_DT_GET(CLOCK_A_NODE),
		.subsys = UINT_TO_POINTER(0x44U),
	};
	uint32_t init_sequence = 0U;

	clock_a_data.on_result = -EIO;
	zassert_equal(dma_nxp_sdma_clocked_init(&clock, fake_sdma_init, &init_sequence), -EIO);
	zassert_equal(init_sequence, 0U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.on_subsys), 0x44U);
}

ZTEST(nxp_audio_clock, test_sdma_enables_clock_before_hardware_init)
{
	const struct nxp_clock_dt_spec clock = {
		.dev = DEVICE_DT_GET(CLOCK_A_NODE),
		.subsys = UINT_TO_POINTER(0x33U),
	};
	uint32_t init_sequence = 0U;

	zassert_ok(dma_nxp_sdma_clocked_init(&clock, fake_sdma_init, &init_sequence));
	zassert_equal(clock_a_data.sequence, 1U);
	zassert_equal(init_sequence, 2U);
}

ZTEST(nxp_audio_clock, test_micfil_uses_queried_root_rate_and_subsystem)
{
	const struct nxp_clock_dt_spec clock = {
		.dev = DEVICE_DT_GET(CLOCK_B_NODE),
		.subsys = UINT_TO_POINTER(0x66U),
	};
	uint32_t rate = 0U;

	zassert_ok(dai_nxp_micfil_clock_prepare(&clock, &rate));
	zassert_equal(rate, 196608000U);
	zassert_equal(clock_b_data.on_count, 1U);
	zassert_equal(clock_b_data.get_rate_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.on_subsys), 0x66U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.get_rate_subsys), 0x66U);
}

ZTEST(nxp_audio_clock, test_micfil_preserves_rate_error_during_rollback)
{
	const struct nxp_clock_dt_spec clock = {
		.dev = DEVICE_DT_GET(CLOCK_A_NODE),
		.subsys = UINT_TO_POINTER(0x77U),
	};
	uint32_t rate = 0U;

	clock_a_data.get_rate_result = -EIO;
	clock_a_data.off_result = -EPERM;
	zassert_equal(dai_nxp_micfil_clock_prepare(&clock, &rate), -EIO);
	zassert_equal(clock_a_data.off_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.off_subsys), 0x77U);
}

ZTEST_SUITE(nxp_audio_clock, NULL, NULL, nxp_audio_clock_before, NULL, NULL);
