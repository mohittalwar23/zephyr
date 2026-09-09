/*
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>

#include <zephyr/shell/shell.h>

#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_latency.h>
#include <zephyr/mpipe/aud/mpipe_aud_loopback_probe.h>
#include <zephyr/mpipe/aud/mpipe_aud.h>
#include <zephyr/mpipe/aud/mpipe_aud_buffer_pool.h>
#include <zephyr/mpipe/aud/mpipe_aud_dmic_src.h>
#include <zephyr/mpipe/aud/mpipe_aud_gain.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_codec_sink.h>
#include <zephyr/mpipe/aud/mpipe_aud_i2s_src.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>
#include <zephyr/mpipe/utils/mpipe_player.h>

/* Frame interval negotiated by this sample. */
#define SAMPLE_FRAME_INTERVAL_US 10000U

LOG_MODULE_REGISTER(main);

enum {
	PIPE_ID,
	AUD_SRC_ID,
	CAPS_FILTER_ID,
	AUD_GAIN_ID,
	I2S_SINK_ID,
};

__nocache struct k_mem_slab mem_slab;

static struct mpipe pipe;
#ifdef CONFIG_SAMPLE_AUDIO_SOURCE_I2S
static struct mpipe_aud_i2s_src source;
#define audio_src_init(src, id)                                                                    \
	mpipe_aud_i2s_src_init((src), (id), DEVICE_DT_GET(DT_ALIAS(i2s_codec_rx)))
#else
static struct mpipe_aud_dmic_src source;
#define audio_src_init(src, id) mpipe_aud_dmic_src_init((src), (id))
#endif
static struct mpipe_caps_filter caps_filter;
static struct mpipe_aud_gain gain;
static struct mpipe_aud_i2s_codec_sink sink;
static struct mpipe_player player;

#if defined(CONFIG_MPIPE_LATENCY)
static int cmd_latency_show(const struct shell *sh, size_t argc, char **argv)
{
	struct mpipe_latency_stats transit;
	struct mpipe_latency_occupancy_stats occ;
#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)
	struct mpipe_aud_loopback_result probe;
#endif

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	mpipe_latency_get_stats(&transit);
	mpipe_latency_occupancy_get_stats(&occ);
#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)
	mpipe_aud_loopback_get(&probe);
#endif

	shell_print(sh, "cycle counter: %u Hz", (unsigned int)sys_clock_hw_cycles_per_sec());

	if (transit.count == 0U) {
		shell_print(sh, "nothing measured yet");
		return 0;
	}

	shell_print(sh, "transit (source push to sink): n=%u  %u/%llu/%u us (min/mean/max)",
		    transit.count, transit.min_us,
		    (unsigned long long)(transit.sum_us / transit.count), transit.max_us);

	if (occ.count > 0U) {
		uint64_t mean_x100 = (occ.sum_blocks * 100U) / occ.count;
		bool off_rate = occ.period_us != 0U &&
				(occ.period_us * 10U < SAMPLE_FRAME_INTERVAL_US * 9U ||
				 occ.period_us * 9U > SAMPLE_FRAME_INTERVAL_US * 10U);

		shell_print(sh,
			    "pool utilisation at handoff: n=%u  %u/%llu.%02llu/%u of %u blocks",
			    occ.count, occ.min_blocks,
			    (unsigned long long)(mean_x100 / 100U),
			    (unsigned long long)(mean_x100 % 100U), occ.max_blocks,
			    occ.capacity);
		shell_print(sh, "buffer handoff period: %u us (negotiated %u us)%s",
			    occ.period_us, SAMPLE_FRAME_INTERVAL_US,
			    off_rate ? "  <-- outside 10% of negotiated interval" : "");
	}

#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)
	shell_print(sh, "loudest captured sample seen: %u", probe.noise_floor);
#endif

	return 0;
}

static int cmd_latency_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	mpipe_latency_reset();
	shell_print(sh, "latency statistics cleared");
	return 0;
}

#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)
static int cmd_latency_loopback(const struct shell *sh, size_t argc, char **argv)
{
	struct mpipe_aud_loopback_result res;
	uint32_t width = gain.bit_width;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (width != 16U && width != 32U) {
		shell_error(sh, "negotiated %u-bit PCM is not supported by the probe", width);
		return -ENOTSUP;
	}

	ret = mpipe_aud_loopback_arm(width, 0U);

	if (ret != 0) {
		shell_error(sh, "arm failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "burst armed at %u-bit: silencing the output to measure the floor,"
			" then one full-scale burst", width);

	/* Silence first, then the burst, so allow for both. */
	for (int i = 0; i < 200; i++) {
		k_msleep(10);
		mpipe_aud_loopback_get(&res);
		if (res.valid || res.timed_out) {
			break;
		}
	}

	mpipe_aud_loopback_get(&res);
	if (res.timed_out) {
		shell_print(sh,
			    "round-trip probe timed out (loudest %u, quiet floor %u); audio restored",
			    res.noise_floor, res.quiet_floor);
		return 0;
	}
	if (!res.valid) {
		shell_print(sh, "round-trip probe still pending");
		return 0;
	}

	shell_print(sh, "round trip through the converters: %u us (peak %u, floor with output"
			" silent %u)", res.round_trip_us, res.peak, res.quiet_floor);

	if (res.hit_total > 0U) {
		/*
		 * Detection lands on a whole buffer; the burst arrived part way
		 * through that buffer's capture window, so take back the part
		 * of the window that came after it.
		 */
		uint32_t tail_us = (uint32_t)(((uint64_t)(res.hit_total - res.hit_index) *
					       SAMPLE_FRAME_INTERVAL_US) / res.hit_total);

		shell_print(sh, "  burst found at sample %u of %u, so %u us of that buffer came"
				" after it", res.hit_index, res.hit_total, tail_us);
		shell_print(sh, "  round trip corrected to sample resolution: %u us",
			    (res.round_trip_us > tail_us) ? (res.round_trip_us - tail_us) : 0U);
	}
	return 0;
}
#endif /* CONFIG_MPIPE_AUD_LOOPBACK_PROBE */

SHELL_STATIC_SUBCMD_SET_CREATE(latency_cmds,
			       SHELL_CMD(show, NULL, "Show the latency figures",
					 cmd_latency_show),
			       SHELL_CMD(reset, NULL, "Clear the statistics", cmd_latency_reset),
#if defined(CONFIG_MPIPE_AUD_LOOPBACK_PROBE)
			       SHELL_CMD_ARG(loopback, NULL,
					     "Measure the round trip through the converters",
					     cmd_latency_loopback, 1, 0),
#endif
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(latency, &latency_cmds, "Pipeline latency", NULL);
#endif /* CONFIG_MPIPE_LATENCY */

int main(void)
{
	int gain_val = CONFIG_SAMPLE_AUDIO_GAIN_PERCENT;
	struct mpipe_structure caps;
	int ret;

	ret = mpipe_pipeline_init(&pipe, PIPE_ID);
	if (ret < 0) {
		goto err;
	}
	ret = audio_src_init(&source, AUD_SRC_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_caps_filter_init(&caps_filter, CAPS_FILTER_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_aud_gain_init(&gain, AUD_GAIN_ID);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_aud_i2s_codec_sink_init(&sink, I2S_SINK_ID);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_object_set_properties((struct mpipe_object *)&source,
					  MPIPE_PROP_AUD_SRC_SLAB_PTR, &mem_slab,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&sink,
					  MPIPE_PROP_AUD_SINK_SLAB_PTR, &mem_slab,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&gain,
					  MPIPE_PROP_AUD_TRANSFORM_GAIN, &gain_val,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

#if CONFIG_SAMPLE_AUDIO_RATE > 0
	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					  MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT, 10000,
					  MPIPE_CAPS_NUM_OF_CHANNEL, MPIPE_TYPE_UINT, 2,
					  MPIPE_CAPS_SAMPLE_RATE, MPIPE_TYPE_UINT,
					  CONFIG_SAMPLE_AUDIO_RATE,
					  MPIPE_CAPS_END);
#else
	ret = mpipe_structure_init_fields(&caps, MPIPE_MEDIA_AUDIO_PCM,
					  MPIPE_CAPS_FRAME_INTERVAL, MPIPE_TYPE_UINT,
					  SAMPLE_FRAME_INTERVAL_US,
					  MPIPE_CAPS_NUM_OF_CHANNEL, MPIPE_TYPE_UINT, 2,
					  MPIPE_CAPS_END);
#endif
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					  MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &caps,
					  MPIPE_PROP_LIST_END);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_bin_add((struct mpipe_bin *)&pipe, (struct mpipe_element *)&source,
			    (struct mpipe_element *)&caps_filter, (struct mpipe_element *)&gain,
			    (struct mpipe_element *)&sink, NULL);
	if (ret < 0) {
		goto err;
	}
	ret = mpipe_element_link((struct mpipe_element *)&source,
				 (struct mpipe_element *)&caps_filter,
				 (struct mpipe_element *)&gain,
				 (struct mpipe_element *)&sink, NULL);
	if (ret < 0) {
		goto err;
	}

	ret = mpipe_player_init(&player, &pipe);
	if (ret < 0) {
		goto err;
	}

	LOG_INF("player ready -- controls: player play | pause | stop | replay | status");
	mpipe_player_play(&player);

	return 0;

err:
	LOG_ERR("Aborting sample (%d)", ret);
	return ret != 0 ? ret : -EIO;
}
