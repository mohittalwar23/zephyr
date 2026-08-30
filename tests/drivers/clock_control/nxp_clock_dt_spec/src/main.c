/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT vnd_nxp_clock_consumer

#include <zephyr/drivers/clock_control/nxp_clock_control.h>
#include <zephyr/ztest.h>

#define CLOCK_A_NODE       DT_NODELABEL(clock_a)
#define CLOCK_B_NODE       DT_NODELABEL(clock_b)
#define CLOCK_UNREADY_NODE DT_NODELABEL(clock_unready)
#define CLOCK_ZERO_NODE    DT_NODELABEL(clock_zero)
#define CONSUMER_NODE      DT_NODELABEL(clock_consumer)
#define NULL_CLOCK_SPEC    {.dev = NULL, .subsys = NULL}

struct fake_clock_data {
	int on_result;
	int off_result;
	int rate_result;
	uint32_t rate;
	clock_control_subsys_t last_subsys;
	uint32_t on_count;
	uint32_t off_count;
	uint32_t rate_count;
};

static int fake_clock_on(const struct device *dev, clock_control_subsys_t subsys)
{
	struct fake_clock_data *data = dev->data;

	data->last_subsys = subsys;
	data->on_count++;

	return data->on_result;
}

static int fake_clock_off(const struct device *dev, clock_control_subsys_t subsys)
{
	struct fake_clock_data *data = dev->data;

	data->last_subsys = subsys;
	data->off_count++;

	return data->off_result;
}

static int fake_clock_get_rate(const struct device *dev, clock_control_subsys_t subsys,
			       uint32_t *rate)
{
	struct fake_clock_data *data = dev->data;

	data->last_subsys = subsys;
	data->rate_count++;
	if (data->rate_result == 0) {
		*rate = data->rate;
	}

	return data->rate_result;
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
static struct fake_clock_data clock_zero_data;

DEVICE_DT_DEFINE(CLOCK_A_NODE, fake_clock_ready_init, NULL, &clock_a_data, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);
DEVICE_DT_DEFINE(CLOCK_B_NODE, fake_clock_ready_init, NULL, &clock_b_data, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);
DEVICE_DT_DEFINE(CLOCK_UNREADY_NODE, fake_clock_unready_init, NULL, &clock_unready_data, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);
DEVICE_DT_DEFINE(CLOCK_ZERO_NODE, fake_clock_ready_init, NULL, &clock_zero_data, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &fake_clock_api);

static const struct nxp_clock_dt_spec indexed_specs[] = {
	NXP_CLOCK_DT_SPEC_GET_BY_IDX(CONSUMER_NODE, 0),
	NXP_CLOCK_DT_SPEC_GET_BY_IDX(CONSUMER_NODE, 1),
	NXP_CLOCK_DT_SPEC_GET_BY_IDX(CONSUMER_NODE, 2),
};
static const struct nxp_clock_dt_spec first_spec = NXP_CLOCK_DT_SPEC_GET(CONSUMER_NODE);
static const struct nxp_clock_dt_spec instance_spec = NXP_CLOCK_DT_SPEC_INST_GET(0);
static const struct nxp_clock_dt_spec instance_indexed_spec =
	NXP_CLOCK_DT_SPEC_INST_GET_BY_IDX(0, 1);
static const struct nxp_clock_dt_spec fallback_spec =
	NXP_CLOCK_DT_SPEC_GET_BY_IDX_OR(CONSUMER_NODE, 3, NULL_CLOCK_SPEC);
static const struct nxp_clock_dt_spec present_or_spec =
	NXP_CLOCK_DT_SPEC_GET_BY_IDX_OR(CONSUMER_NODE, 1, NULL_CLOCK_SPEC);

static void reset_fake_clock(struct fake_clock_data *data, uint32_t rate)
{
	*data = (struct fake_clock_data){
		.rate = rate,
	};
}

static void nxp_clock_dt_spec_before(void *fixture)
{
	ARG_UNUSED(fixture);

	reset_fake_clock(&clock_a_data, 24576000U);
	reset_fake_clock(&clock_b_data, 196608000U);
	reset_fake_clock(&clock_unready_data, 12288000U);
	reset_fake_clock(&clock_zero_data, 24000000U);
}

ZTEST(nxp_clock_dt_spec, test_indexed_specs_preserve_provider_and_name_cell)
{
	zassert_equal(indexed_specs[0].dev, DEVICE_DT_GET(CLOCK_A_NODE));
	zassert_equal(POINTER_TO_UINT(indexed_specs[0].subsys), 0x11U);
	zassert_equal(indexed_specs[1].dev, DEVICE_DT_GET(CLOCK_B_NODE));
	zassert_equal(POINTER_TO_UINT(indexed_specs[1].subsys), 0x22U);
}

ZTEST(nxp_clock_dt_spec, test_zero_cell_provider_uses_zero_subsystem)
{
	zassert_equal(indexed_specs[2].dev, DEVICE_DT_GET(CLOCK_ZERO_NODE));
	zassert_is_null(indexed_specs[2].subsys);
}

ZTEST(nxp_clock_dt_spec, test_convenience_constructors_select_requested_spec)
{
	zassert_equal(first_spec.dev, DEVICE_DT_GET(CLOCK_A_NODE));
	zassert_equal(POINTER_TO_UINT(first_spec.subsys), 0x11U);
	zassert_equal(instance_spec.dev, DEVICE_DT_GET(CLOCK_A_NODE));
	zassert_equal(POINTER_TO_UINT(instance_spec.subsys), 0x11U);
	zassert_equal(instance_indexed_spec.dev, DEVICE_DT_GET(CLOCK_B_NODE));
	zassert_equal(POINTER_TO_UINT(instance_indexed_spec.subsys), 0x22U);
	zassert_is_null(fallback_spec.dev);
	zassert_is_null(fallback_spec.subsys);
}

ZTEST(nxp_clock_dt_spec, test_indexed_or_constructor_selects_existing_spec)
{
	zassert_equal(present_or_spec.dev, DEVICE_DT_GET(CLOCK_B_NODE));
	zassert_equal(POINTER_TO_UINT(present_or_spec.subsys), 0x22U);
}

ZTEST(nxp_clock_dt_spec, test_wrappers_forward_provider_and_subsystem)
{
	uint32_t rate = 0U;

	zassert_ok(nxp_clock_control_on_dt(&indexed_specs[0]));
	zassert_equal(clock_a_data.on_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.last_subsys), 0x11U);

	zassert_ok(nxp_clock_control_off_dt(&indexed_specs[1]));
	zassert_equal(clock_b_data.off_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.last_subsys), 0x22U);

	zassert_ok(nxp_clock_control_get_rate_dt(&indexed_specs[1], &rate));
	zassert_equal(clock_b_data.rate_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.last_subsys), 0x22U);
	zassert_equal(rate, 196608000U);
}

ZTEST(nxp_clock_dt_spec, test_wrapper_forwards_zero_subsystem_to_zero_cell_provider)
{
	zassert_ok(nxp_clock_control_on_dt(&indexed_specs[2]));
	zassert_equal(clock_zero_data.on_count, 1U);
	zassert_is_null(clock_zero_data.last_subsys);
}

/* Catches removal of the NULL-specification -ENODEV gate. */
ZTEST(nxp_clock_dt_spec, test_wrappers_reject_null_spec_before_backend_dispatch)
{
	uint32_t rate = 0xa5a5a5a5U;

	zassert_equal(nxp_clock_control_on_dt(NULL), -ENODEV);
	zassert_equal(nxp_clock_control_off_dt(NULL), -ENODEV);
	zassert_equal(nxp_clock_control_get_rate_dt(NULL, &rate), -ENODEV);
	zassert_equal(rate, 0xa5a5a5a5U);
	zassert_equal(clock_a_data.on_count, 0U);
	zassert_equal(clock_b_data.off_count, 0U);
	zassert_equal(clock_b_data.rate_count, 0U);
}

/* Catches removal of the missing-provider -ENODEV gate. */
ZTEST(nxp_clock_dt_spec, test_wrappers_reject_missing_default_spec)
{
	uint32_t rate = 0x5a5a5a5aU;

	zassert_equal(nxp_clock_control_on_dt(&fallback_spec), -ENODEV);
	zassert_equal(nxp_clock_control_off_dt(&fallback_spec), -ENODEV);
	zassert_equal(nxp_clock_control_get_rate_dt(&fallback_spec, &rate), -ENODEV);
	zassert_equal(rate, 0x5a5a5a5aU);
	zassert_equal(clock_a_data.on_count, 0U);
	zassert_equal(clock_b_data.off_count, 0U);
	zassert_equal(clock_b_data.rate_count, 0U);
}

/* Catches a wrapper mutation that discards a backend operation error. */
ZTEST(nxp_clock_dt_spec, test_wrappers_propagate_backend_errors)
{
	uint32_t rate = 0xc3c3c3c3U;

	clock_a_data.on_result = -EACCES;
	clock_b_data.off_result = -EPERM;
	clock_b_data.rate_result = -EIO;

	zassert_equal(nxp_clock_control_on_dt(&indexed_specs[0]), -EACCES);
	zassert_equal(clock_a_data.on_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_a_data.last_subsys), 0x11U);

	zassert_equal(nxp_clock_control_off_dt(&indexed_specs[1]), -EPERM);
	zassert_equal(clock_b_data.off_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.last_subsys), 0x22U);

	zassert_equal(nxp_clock_control_get_rate_dt(&indexed_specs[1], &rate), -EIO);
	zassert_equal(clock_b_data.rate_count, 1U);
	zassert_equal(POINTER_TO_UINT(clock_b_data.last_subsys), 0x22U);
	zassert_equal(rate, 0xc3c3c3c3U);
}

ZTEST(nxp_clock_dt_spec, test_wrappers_reject_missing_or_unready_provider)
{
	const struct nxp_clock_dt_spec unready = {
		.dev = DEVICE_DT_GET(CLOCK_UNREADY_NODE),
		.subsys = UINT_TO_POINTER(0x33U),
	};
	uint32_t rate;

	zassert_false(nxp_clock_is_ready_dt(NULL));
	zassert_false(nxp_clock_is_ready_dt(&fallback_spec));
	zassert_false(nxp_clock_is_ready_dt(&unready));
	zassert_equal(nxp_clock_control_on_dt(&unready), -ENODEV);
	zassert_equal(nxp_clock_control_off_dt(&unready), -ENODEV);
	zassert_equal(nxp_clock_control_get_rate_dt(&unready, &rate), -ENODEV);
	zassert_equal(clock_unready_data.on_count, 0U);
	zassert_equal(clock_unready_data.off_count, 0U);
	zassert_equal(clock_unready_data.rate_count, 0U);
}

ZTEST_SUITE(nxp_clock_dt_spec, NULL, NULL, nxp_clock_dt_spec_before, NULL, NULL);
