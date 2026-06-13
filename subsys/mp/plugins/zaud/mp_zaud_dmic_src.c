/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <zephyr/mp/core/mp_buffer.h>
#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_src.h>
#include <zephyr/mp/core/mp_structure.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>
#include <zephyr/mp/zaud/mp_zaud_buffer_pool.h>
#include <zephyr/mp/zaud/mp_zaud_dmic_src.h>
#include <zephyr/mp/zaud/mp_zaud_src.h>

LOG_MODULE_REGISTER(mp_zaud_dmic_src, CONFIG_MP_LOG_LEVEL);

#if !DT_NODE_EXISTS(DT_ALIAS(dmic_dev))
#error "dmic-dev devicetree alias not found. Add it to your board overlay."
#endif

/* TODO: device is hardcoded via DT alias, limiting this element to a single
 * instance. Make it a per-instance property to support multiple DMIC sources.
 */
#define ZAUD_DMIC_DEV       DEVICE_DT_GET(DT_ALIAS(dmic_dev))
/* Max channels addressable by the PDM channel map (req_chan_map_lo) */
#define ZAUD_DMIC_MAX_CHANNELS 8
#define ZAUD_DMIC_NUM_BLOCKS CONFIG_MP_PLUGIN_ZAUD_NUM_BLOCKS
#define ZAUD_DMIC_MAX_FRAME  CONFIG_MP_PLUGIN_ZAUD_MAX_FRAME_SIZE
#define ZAUD_DMIC_READ_TIMEOUT_MS 1000

/* DMA-capable backing for the DMIC driver's mem slab */
static char __nocache __aligned(sizeof(void *))
	zaud_dmic_slab_backing[ZAUD_DMIC_NUM_BLOCKS * ZAUD_DMIC_MAX_FRAME];
static struct k_mem_slab zaud_dmic_slab;

/* libMP transport buffers (carry a copy of each captured frame downstream) */
NET_BUF_POOL_FIXED_DEFINE(zaud_dmic_nb_pool, ZAUD_DMIC_NUM_BLOCKS + 2, ZAUD_DMIC_MAX_FRAME,
			  sizeof(struct mp_buffer_meta), mp_buffer_destroy);

static bool mp_zaud_dmic_src_set_caps(struct mp_src *src, struct mp_caps *caps)
{
	struct mp_zaud_dmic_src *dmic_src = MP_ZAUD_DMIC_SRC(src);
	struct mp_structure *s = mp_caps_get_structure(caps, 0);

	int sample_rate = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_SAMPLE_RATE));
	int bit_width = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_BITWIDTH));
	int num_of_channel = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_NUM_OF_CHANNEL));
	int frame_interval = mp_value_get_int(mp_structure_get_value(s, MP_CAPS_FRAME_INTERVAL));

	struct pcm_stream_cfg stream = {
		.pcm_width = bit_width,
		.mem_slab = dmic_src->pool.mem_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = num_of_channel,
		},
	};

	/* Pair channels on the same PDM controller with opposite L/R assignment */
	for (int i = 0; i < num_of_channel && i < ZAUD_DMIC_MAX_CHANNELS; i++) {
		cfg.channel.req_chan_map_lo |=
			dmic_build_channel_map(i, i / 2, (i % 2) ? PDM_CHAN_RIGHT : PDM_CHAN_LEFT);
	}

	cfg.streams[0].pcm_rate = sample_rate;
	cfg.streams[0].block_size =
		mp_zaud_frame_size(bit_width, sample_rate, frame_interval, num_of_channel);

	LOG_DBG("DMIC configure: rate=%u width=%u ch=%u block=%u", sample_rate, bit_width,
		num_of_channel, cfg.streams[0].block_size);

	if (dmic_configure(dmic_src->pool.zaud_dev, &cfg) < 0) {
		LOG_ERR("Failed to configure DMIC");
		return false;
	}

	mp_caps_replace(&src->srcpad.caps, caps);

	return true;
}

static int mp_zaud_dmic_src_acquire_buffer(struct mp_buffer_pool *pool, struct net_buf **buf)
{
	struct mp_zaud_buffer_pool *zaud_pool = MP_ZAUD_BUFFER_POOL(pool);
	struct mp_buffer_meta *meta;
	struct net_buf *nb;
	void *block = NULL;
	size_t size = zaud_pool->frame_size;
	int err;

	err = dmic_read(zaud_pool->zaud_dev, 0, &block, &size, ZAUD_DMIC_READ_TIMEOUT_MS);
	if (err < 0) {
		/*
		 * On native_sim an exhausted/missing input file is the only
		 * realistic error, so any failure is treated as end of stream.
		 * TODO: on real DMIC hardware a transient -EAGAIN/-EIO must be
		 * surfaced as a device error rather than EOS; narrow this to a
		 * specific sentinel once PR2/PR3 bring real capture hardware in.
		 */
		LOG_DBG("dmic_read returned %d, signalling EOS", err);
		return -ENODATA;
	}

	nb = net_buf_alloc(pool->nb_pool, K_NO_WAIT);
	if (nb == NULL) {
		LOG_ERR("No free transport buffer");
		k_mem_slab_free(zaud_pool->mem_slab, block);
		return -ENOMEM;
	}

	if (size > net_buf_tailroom(nb)) {
		LOG_ERR("Frame size %zu exceeds transport buffer capacity %zu", size,
			net_buf_tailroom(nb));
		net_buf_unref(nb);
		k_mem_slab_free(zaud_pool->mem_slab, block);
		return -EINVAL;
	}

	net_buf_add_mem(nb, block, size);
	k_mem_slab_free(zaud_pool->mem_slab, block);

	meta = mp_buffer_get_meta(nb);
	meta->pool = pool;
	meta->bytes_used = size;

	*buf = nb;

	return 0;
}

static int mp_zaud_dmic_src_release_buffer(struct mp_buffer_pool *pool, struct net_buf *buf)
{
	/* Transport buffer data is owned by the net_buf itself; nothing to do. */
	ARG_UNUSED(pool);
	ARG_UNUSED(buf);
	return 0;
}

static int mp_zaud_dmic_src_start(struct mp_buffer_pool *pool)
{
	struct mp_zaud_buffer_pool *zaud_pool = MP_ZAUD_BUFFER_POOL(pool);

	if (dmic_trigger(zaud_pool->zaud_dev, DMIC_TRIGGER_START) < 0) {
		LOG_ERR("Unable to start DMIC capture");
		return -EIO;
	}

	LOG_INF("DMIC capture started");

	return 0;
}

void mp_zaud_dmic_src_init(struct mp_element *self)
{
	struct mp_src *src = MP_SRC(self);
	struct mp_zaud_dmic_src *dmic_src = MP_ZAUD_DMIC_SRC(self);
	struct mp_zaud_buffer_pool *pool = &dmic_src->pool;

	/* Init base audio source */
	mp_zaud_src_init(self);

	/* Wire the buffer pool to this element */
	src->pool = &pool->pool;
	mp_zaud_buffer_pool_init(src->pool);

	pool->zaud_dev = ZAUD_DMIC_DEV;
	pool->mem_slab = &zaud_dmic_slab;
	pool->slab_backing = zaud_dmic_slab_backing;
	pool->slab_backing_size = sizeof(zaud_dmic_slab_backing);
	pool->num_blocks = ZAUD_DMIC_NUM_BLOCKS;
	src->pool->nb_pool = &zaud_dmic_nb_pool;

	/*
	 * Wire the element's function pointers unconditionally, before the
	 * device-readiness check. If they were skipped on a not-ready device,
	 * acquire_buffer/set_caps would stay NULL and the core would silently
	 * push NULL buffers and post a clean EOS having captured nothing. With
	 * them wired, a not-ready device instead fails loudly during
	 * negotiation (set_caps -> dmic_configure) or pool start.
	 */
	dmic_src->zaud_src.get_audio_caps = dmic_get_caps;

	src->set_caps = mp_zaud_dmic_src_set_caps;
	src->pool->acquire_buffer = mp_zaud_dmic_src_acquire_buffer;
	src->pool->release_buffer = mp_zaud_dmic_src_release_buffer;
	src->pool->start = mp_zaud_dmic_src_start;

	if (!device_is_ready(pool->zaud_dev)) {
		LOG_ERR("%s is not ready", pool->zaud_dev->name);
		return;
	}

	/* Populate supported caps from the driver for negotiation */
	mp_src_update_caps(src, src->get_caps(src));
}
