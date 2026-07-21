/*
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/irq.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/clock_control.h>
#include "fsl_sdma.h"

LOG_MODULE_REGISTER(nxp_sdma);

#define DMA_NXP_SDMA_BD_COUNT CONFIG_DMA_NXP_SDMA_BD_COUNT
#define DMA_NXP_SDMA_CHAN_DEFAULT_PRIO 4

/*
 * Fallback peripheral FIFO watermark, in bytes. Used when the caller leaves the
 * relevant burst length unset, which is what every user of this driver did
 * before the burst length was honoured.
 */
#define DMA_NXP_SDMA_DEFAULT_WATERMARK 64

#define DT_DRV_COMPAT nxp_sdma

AT_NONCACHEABLE_SECTION_ALIGN(static sdma_context_data_t
			      sdma_contexts[FSL_FEATURE_SDMA_MODULE_CHANNEL], 4);

struct sdma_dev_cfg {
	SDMAARM_Type *base;
	void (*irq_config)(void);
	const struct device *clk_dev;
	uint32_t bus_clk_id;
};

struct sdma_channel_data {
	sdma_handle_t handle;
	sdma_transfer_config_t transfer_cfg;
	sdma_peripheral_t peripheral;
	uint32_t direction;
	uint32_t index;
	const struct device *dev;
	sdma_buffer_descriptor_t *bd_pool; /*pre-allocated list of BD used for transfer */
	uint32_t bd_count; /* number of bd */
	uint32_t capacity; /* total transfer capacity for this channel */
	struct dma_config *dma_cfg;
	uint32_t event_source; /* DMA REQ number that trigger this channel */
	struct dma_status stat;
	uint32_t bd_write_idx; /* next BD to reprogram on reload() */
	sdma_transfer_size_t bus_width; /* remembered from config() for reload() */
	/*
	 * Memory-side address each BD was last programmed with, kept in the
	 * caller's address space. SDMA_ConfigBufferDescriptor() may store a
	 * translated address in the descriptor itself (TCM aliases), so the
	 * descriptor cannot be compared against what the caller passes us.
	 */
	uint32_t bd_mem_addr[DMA_NXP_SDMA_BD_COUNT];
	uint32_t bd_size[DMA_NXP_SDMA_BD_COUNT]; /* bytes each BD was given */
	/*
	 * Set the first time reload() is handed an address the channel was not
	 * already pointing at, i.e. the caller queues a distinct block per
	 * transfer rather than cycling a ring set up once at config() time.
	 * Derived rather than configured so that ring users are unaffected.
	 */
	bool queue_mode;
	uint32_t bd_pending; /* queued descriptors not yet completed */
	bool started; /* channel is between start() and stop() */

	void *arg; /* argument passed to user-defined DMA callback */
	dma_callback_t cb; /* user-defined callback for DMA transfer completion */
};

struct sdma_dev_data {
	struct dma_context dma_ctx;
	atomic_t *channels_atomic;
	struct sdma_channel_data chan[FSL_FEATURE_SDMA_MODULE_CHANNEL];
	sdma_buffer_descriptor_t bd_pool[FSL_FEATURE_SDMA_MODULE_CHANNEL][DMA_NXP_SDMA_BD_COUNT]
		__aligned(64);
};

static int dma_nxp_sdma_init_stat(struct sdma_channel_data *chan_data)
{
	chan_data->stat.read_position = 0;
	chan_data->stat.write_position = 0;

	switch (chan_data->direction) {
	case MEMORY_TO_PERIPHERAL:
		/* buffer is full */
		chan_data->stat.pending_length = chan_data->capacity;
		chan_data->stat.free = 0;
		break;
	case PERIPHERAL_TO_MEMORY:
		/* buffer is empty */
		chan_data->stat.pending_length = 0;
		chan_data->stat.free = chan_data->capacity;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int dma_nxp_sdma_consume(struct sdma_channel_data *chan_data, uint32_t bytes)
{
	if (bytes > chan_data->stat.pending_length)
		return -EINVAL;

	chan_data->stat.read_position += bytes;
	chan_data->stat.read_position %= chan_data->capacity;

	if (chan_data->stat.read_position > chan_data->stat.write_position) {
		chan_data->stat.free = chan_data->stat.read_position -
			chan_data->stat.write_position;
	} else {
		chan_data->stat.free = chan_data->capacity -
			(chan_data->stat.write_position - chan_data->stat.read_position);
	}

	chan_data->stat.pending_length = chan_data->capacity - chan_data->stat.free;

	return 0;
}

static int dma_nxp_sdma_produce(struct sdma_channel_data *chan_data, uint32_t bytes)
{
	if (bytes > chan_data->stat.free)
		return -EINVAL;

	chan_data->stat.write_position += bytes;
	chan_data->stat.write_position %= chan_data->capacity;

	if (chan_data->stat.write_position > chan_data->stat.read_position) {
		chan_data->stat.pending_length = chan_data->stat.write_position -
			chan_data->stat.read_position;
	} else {
		chan_data->stat.pending_length = chan_data->capacity -
			(chan_data->stat.read_position - chan_data->stat.write_position);
	}

	chan_data->stat.free = chan_data->capacity - chan_data->stat.pending_length;

	return 0;
}

static void dma_nxp_sdma_isr(const void *data)
{
	uint32_t val;
	uint32_t i = 1;
	struct sdma_channel_data *chan_data;
	struct device *dev = (struct device *)data;
	struct sdma_dev_data *dev_data = dev->data;
	const struct sdma_dev_cfg *dev_cfg = dev->config;

	/* Clear channel 0 */
	SDMA_ClearChannelInterruptStatus(dev_cfg->base, 1U);

	/* Ignore channel 0, is used only for download */
	val = SDMA_GetChannelInterruptStatus(dev_cfg->base) >> 1U;
	while (val) {
		if ((val & 0x1) != 0) {
			chan_data = &dev_data->chan[i];
			SDMA_ClearChannelInterruptStatus(dev_cfg->base, 1 << i);
			SDMA_HandleIRQ(&chan_data->handle);

			if (chan_data->cb)
				chan_data->cb(chan_data->dev, chan_data->arg, i, DMA_STATUS_BLOCK);
		}
		i++;
		val >>= 1;
	}
}

void sdma_set_transfer_type(struct dma_config *config, sdma_transfer_type_t *type)
{
	switch (config->channel_direction) {
	case MEMORY_TO_MEMORY:
		*type = kSDMA_MemoryToMemory;
		break;
	case MEMORY_TO_PERIPHERAL:
		*type = kSDMA_MemoryToPeripheral;
		break;
	case PERIPHERAL_TO_MEMORY:
		*type = kSDMA_PeripheralToMemory;
		break;
	case PERIPHERAL_TO_PERIPHERAL:
		*type = kSDMA_PeripheralToPeripheral;
		break;
	default:
		LOG_ERR("%s: channel direction not supported %d", __func__,
			config->channel_direction);
		return;
	}
	LOG_DBG("%s: dir %d type = %d", __func__, config->channel_direction, *type);
}

int sdma_set_peripheral_type(struct dma_config *config, sdma_peripheral_t *type)
{
	switch (config->dma_slot) {
	case kSDMA_PeripheralNormal:
	case kSDMA_PeripheralNormal_SP:
	case kSDMA_PeripheralMultiFifoPDM:
	case kSDMA_PeripheralMultiFifoSaiRX:
	case kSDMA_PeripheralMultiFifoSaiTX:
		*type = config->dma_slot;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

void dma_nxp_sdma_callback(sdma_handle_t *handle, void *userData, bool TransferDone,
			   uint32_t bdIndex)
{
	const struct sdma_dev_cfg *dev_cfg;
	struct sdma_channel_data *chan_data = userData;
	sdma_buffer_descriptor_t *bd;
	int xfer_size;

	dev_cfg = chan_data->dev->config;

	/*
	 * Ring users hand over equally sized blocks once, so the size of the one
	 * that just completed can be derived. Queued blocks are whatever size
	 * the caller asked for, so it has to be remembered per descriptor.
	 */
	if (chan_data->queue_mode) {
		xfer_size = chan_data->bd_size[bdIndex];
	} else {
		xfer_size = chan_data->capacity / chan_data->bd_count;
	}

	switch (chan_data->direction) {
	case MEMORY_TO_PERIPHERAL:
		dma_nxp_sdma_consume(chan_data, xfer_size);
		break;
	case PERIPHERAL_TO_MEMORY:
		dma_nxp_sdma_produce(chan_data, xfer_size);
		break;
	default:
		break;
	}

	if (chan_data->queue_mode) {
		/*
		 * The block that just completed belongs to the caller and is not
		 * ours to replay: re-arming this descriptor would transmit a
		 * buffer that has already been consumed, or receive into one the
		 * caller has taken back. Restart only if reload() has since
		 * queued a further block, and let the channel go idle otherwise
		 * so the peripheral reports a genuine underrun.
		 */
		if (chan_data->bd_pending > 0) {
			chan_data->bd_pending--;
		}

		if (chan_data->bd_pending == 0) {
			return;
		}
	} else {
		/* prepare next BD for transfer */
		bd = &chan_data->bd_pool[bdIndex];
		bd->count = xfer_size;
		bd->status |= (uint8_t)kSDMA_BDStatusDone;
	}

	SDMA_StartChannelSoftware(dev_cfg->base, chan_data->index);
}

static int dma_nxp_sdma_channel_init(const struct device *dev, uint32_t channel)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;

	chan_data = &dev_data->chan[channel];
	SDMA_CreateHandle(&chan_data->handle, dev_cfg->base, channel, &sdma_contexts[channel]);

	SDMA_SetCallback(&chan_data->handle, dma_nxp_sdma_callback, chan_data);

	return 0;
}

static void dma_nxp_sdma_setup_bd(const struct device *dev, uint32_t channel,
				struct dma_config *config)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	sdma_buffer_descriptor_t *crt_bd;
	struct dma_block_config *block_cfg;
	int i;

	chan_data = &dev_data->chan[channel];

	chan_data->capacity = 0;

	/* initialize bd pool */
	chan_data->bd_pool = &dev_data->bd_pool[channel][0];
	chan_data->bd_count = config->block_count;
	chan_data->bd_write_idx = 0;
	chan_data->bus_width = config->source_data_size;
	chan_data->queue_mode = false;
	chan_data->bd_pending = 0;

	memset(chan_data->bd_pool, 0, sizeof(sdma_buffer_descriptor_t) * chan_data->bd_count);
	SDMA_InstallBDMemory(&chan_data->handle, chan_data->bd_pool, chan_data->bd_count);

	crt_bd = chan_data->bd_pool;
	block_cfg = config->head_block;

	for (i = 0; i < config->block_count; i++) {
		bool is_last = false;
		bool is_wrap = false;

		if (i == config->block_count - 1) {
			is_last = true;
			is_wrap = true;
		}

		SDMA_ConfigBufferDescriptor(crt_bd,
			block_cfg->source_address, block_cfg->dest_address,
			config->source_data_size, block_cfg->block_size,
			is_last, true, is_wrap, chan_data->transfer_cfg.type);

		chan_data->bd_mem_addr[i] = chan_data->direction == PERIPHERAL_TO_MEMORY
						    ? block_cfg->dest_address
						    : block_cfg->source_address;

		chan_data->capacity += block_cfg->block_size;
		block_cfg = block_cfg->next_block;
		crt_bd++;
	}
}

static int dma_nxp_sdma_config(const struct device *dev, uint32_t channel,
			       struct dma_config *config)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	struct dma_block_config *block_cfg;
	uint32_t watermark;
	int ret;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		LOG_ERR("sdma_config() invalid channel %d", channel);
		return -EINVAL;
	}

	dma_nxp_sdma_channel_init(dev, channel);

	chan_data = &dev_data->chan[channel];
	chan_data->dev = dev;
	chan_data->direction = config->channel_direction;

	chan_data->cb = config->dma_callback;
	chan_data->arg = config->user_data;

	sdma_set_transfer_type(config, &chan_data->transfer_cfg.type);

	ret = sdma_set_peripheral_type(config, &chan_data->peripheral);
	if (ret < 0) {
		LOG_ERR("%s: failed to set peripheral type", __func__);
		return ret;
	}

	if (chan_data->peripheral == kSDMA_PeripheralMultiFifoPDM) {
		unsigned int n_fifos = 4; /* TODO: make this configurable */

		SDMA_SetMultiFifoConfig(&chan_data->transfer_cfg, n_fifos, 0);
		SDMA_EnableSwDone(dev_cfg->base, &chan_data->transfer_cfg, 0,
				  chan_data->peripheral);
	}

	dma_nxp_sdma_setup_bd(dev, channel, config);
	ret = dma_nxp_sdma_init_stat(chan_data);
	if (ret < 0) {
		LOG_ERR("%s: failed to init stat", __func__);
		return ret;
	}

	block_cfg = config->head_block;

	/*
	 * The watermark is the peripheral's FIFO trigger level, so take it from
	 * the burst length the caller gave for whichever side the peripheral is
	 * on. A watermark that disagrees with the peripheral's own FIFO setting
	 * makes it request service at the wrong depth, so this cannot stay
	 * fixed once the driver serves more than one peripheral.
	 */
	watermark = chan_data->direction == PERIPHERAL_TO_MEMORY ? config->source_burst_length
								 : config->dest_burst_length;
	if (watermark == 0) {
		watermark = DMA_NXP_SDMA_DEFAULT_WATERMARK;
	}

	/* prepare first block for transfer ...*/
	SDMA_PrepareTransfer(&chan_data->transfer_cfg,
			     block_cfg->source_address,
			     block_cfg->dest_address,
			     config->source_data_size, config->dest_data_size,
			     watermark,
			     block_cfg->block_size, chan_data->event_source,
			     chan_data->peripheral, chan_data->transfer_cfg.type);

	/*... and submit it to SDMA engine.
	 * Note that SDMA transfer is later manually started by the dma_nxp_sdma_start()
	 */
	chan_data->transfer_cfg.isEventIgnore = false;
	chan_data->transfer_cfg.isSoftTriggerIgnore = false;
	SDMA_SubmitTransfer(&chan_data->handle, &chan_data->transfer_cfg);

	return 0;
}

static int dma_nxp_sdma_start(const struct device *dev, uint32_t channel)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		LOG_ERR("%s: invalid channel %d", __func__, channel);
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];

	SDMA_SetChannelPriority(dev_cfg->base, channel, DMA_NXP_SDMA_CHAN_DEFAULT_PRIO);
	chan_data->started = true;
	SDMA_StartChannelSoftware(dev_cfg->base, channel);

	return 0;
}

static int dma_nxp_sdma_stop(const struct device *dev, uint32_t channel)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		LOG_ERR("%s: invalid channel %d", __func__, channel);
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];

	chan_data->started = false;
	SDMA_StopTransfer(&chan_data->handle);
	return 0;
}

static int dma_nxp_sdma_get_status(const struct device *dev, uint32_t channel,
				   struct dma_status *stat)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	unsigned int key;

	chan_data = &dev_data->chan[channel];

	key = irq_lock();
	stat->free = chan_data->stat.free;
	stat->pending_length = chan_data->stat.pending_length;
	irq_unlock(key);

	return 0;
}

static bool dma_nxp_sdma_bd_needs_reprogram(struct sdma_channel_data *chan_data,
					    uint32_t src, uint32_t dst)
{
	uint32_t mem_addr;

	/*
	 * Once a caller has been seen queueing its own blocks it keeps doing so,
	 * and the decision must not be revisited per call: buffers come from a
	 * pool, so a recycled block can legitimately carry the same address the
	 * descriptor already holds. Treating that as "nothing moved" would skip
	 * both the reprogram and the restart, stalling the channel for good.
	 */
	if (chan_data->queue_mode) {
		return true;
	}

	/* the memory side is the only one a caller can move between transfers */
	mem_addr = chan_data->direction == PERIPHERAL_TO_MEMORY ? dst : src;

	return mem_addr != chan_data->bd_mem_addr[chan_data->bd_write_idx];
}

static int dma_nxp_sdma_reload(const struct device *dev, uint32_t channel, uint32_t src,
			       uint32_t dst, size_t size)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	unsigned int key;

	chan_data = &dev_data->chan[channel];

	if (!size) {
		return 0;
	}

	key = irq_lock();

	/*
	 * Callers that stream a fixed ring (the buffers were handed over once at
	 * config() time) pass back the same addresses every time and only need
	 * the produced/consumed counters advanced. Callers that queue a fresh
	 * block per transfer, such as the I2S API, hand in a new address here,
	 * so the descriptor has to be rewritten to point at it. Detect the
	 * latter by comparing against the descriptor we would reuse: if nothing
	 * moved, this is a no-op and ring users keep their old behaviour.
	 */
	if (dma_nxp_sdma_bd_needs_reprogram(chan_data, src, dst)) {
		sdma_buffer_descriptor_t *bd = &chan_data->bd_pool[chan_data->bd_write_idx];
		bool is_wrap = chan_data->bd_write_idx == (chan_data->bd_count - 1);

		/*
		 * Never mark a queued descriptor as last: that flag halts the
		 * channel when it is reached, which for a caller streaming
		 * continuously would stop the transfer every time the ring wraps,
		 * however much work is still queued behind it. Only the wrap flag
		 * belongs here, to send the engine back to the first descriptor.
		 */
		SDMA_ConfigBufferDescriptor(bd, src, dst, chan_data->bus_width, size,
					    false, true, is_wrap,
					    chan_data->transfer_cfg.type);

		chan_data->bd_mem_addr[chan_data->bd_write_idx] =
			chan_data->direction == PERIPHERAL_TO_MEMORY ? dst : src;
		chan_data->bd_size[chan_data->bd_write_idx] = size;
		chan_data->bd_write_idx = (chan_data->bd_write_idx + 1) % chan_data->bd_count;

		chan_data->queue_mode = true;
		chan_data->bd_pending++;

		/*
		 * The completion callback runs before the client is told a block
		 * is done, so the client queues the next one from underneath it,
		 * by which point the callback has already decided not to restart
		 * the channel. Kick it from here instead: this is the only point
		 * at which a newly queued descriptor is known to exist.
		 */
		if (chan_data->started) {
			SDMA_StartChannelSoftware(dev_cfg->base, chan_data->index);
		}
	}

	if (chan_data->direction == MEMORY_TO_PERIPHERAL) {
		dma_nxp_sdma_produce(chan_data, size);
	} else {
		dma_nxp_sdma_consume(chan_data, size);
	}
	irq_unlock(key);

	return 0;
}

static int dma_nxp_sdma_get_attribute(const struct device *dev, uint32_t type, uint32_t *val)
{
	switch (type) {
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
		*val = 4;
		break;
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
		*val = 128; /* should be dcache_align */
		break;
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*val = DMA_NXP_SDMA_BD_COUNT;
		break;
	default:
		LOG_ERR("invalid attribute type: %d", type);
		return -EINVAL;
	}
	return 0;
}

static bool sdma_channel_filter(const struct device *dev, int chan_id, void *param)
{
	struct sdma_dev_data *dev_data = dev->data;

	/* chan 0 is reserved for boot channel */
	if (chan_id == 0) {
		return false;
	}

	if (chan_id >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		return false;
	}

	dev_data->chan[chan_id].event_source = *((int *)param);
	dev_data->chan[chan_id].index = chan_id;
	dev_data->chan[chan_id].capacity = 0;

	if (pm_device_runtime_get(dev) < 0) {
		LOG_ERR("failed to runtime get");
		return false;
	}

	return true;
}

static void sdma_channel_release(const struct device *dev, uint32_t chan_id)
{
	if (chan_id == 0) {
		return;
	}

	if (chan_id >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		return;
	}

	if (pm_device_runtime_put(dev) < 0) {
		LOG_ERR("failed to runtime put");
		return;
	}
}

static int sdma_bus_clk_enable_disable(const struct device *dev, bool enable)
{
	const struct sdma_dev_cfg *cfg = dev->config;

	if (!cfg->clk_dev) {
		return -ENODEV;
	}

	if (enable) {
		return clock_control_on(cfg->clk_dev, UINT_TO_POINTER(cfg->bus_clk_id));
	} else {
		return clock_control_off(cfg->clk_dev, UINT_TO_POINTER(cfg->bus_clk_id));
	}
}

__maybe_unused static int sdma_pm_action(const struct device *dev, enum pm_device_action action)
{
	bool enable = true;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		enable = false;
		break;
	case PM_DEVICE_ACTION_TURN_ON:
	case PM_DEVICE_ACTION_TURN_OFF:
		return 0;
	}

	return sdma_bus_clk_enable_disable(dev, enable);
}

static DEVICE_API(dma, sdma_api) = {
	.reload = dma_nxp_sdma_reload,
	.config = dma_nxp_sdma_config,
	.start = dma_nxp_sdma_start,
	.stop = dma_nxp_sdma_stop,
	.suspend = dma_nxp_sdma_stop,
	.resume = dma_nxp_sdma_start,
	.get_status = dma_nxp_sdma_get_status,
	.get_attribute = dma_nxp_sdma_get_attribute,
	.chan_filter = sdma_channel_filter,
	.chan_release = sdma_channel_release,
};

static int dma_nxp_sdma_init(const struct device *dev)
{
	struct sdma_dev_data *data = dev->data;
	const struct sdma_dev_cfg *cfg = dev->config;
	sdma_config_t defconfig;

	data->dma_ctx.magic = DMA_MAGIC;
	data->dma_ctx.dma_channels = FSL_FEATURE_SDMA_MODULE_CHANNEL;
	data->dma_ctx.atomic = data->channels_atomic;

	SDMA_GetDefaultConfig(&defconfig);
	defconfig.ratio = kSDMA_ARMClockFreq;

	/* this also ungates the bus clock */
	SDMA_Init(cfg->base, &defconfig);

	/* configure interrupts */
	cfg->irq_config();

	return pm_device_runtime_enable(dev);
}

#define DMA_NXP_SDMA_INIT(inst)                                                                    \
	static ATOMIC_DEFINE(dma_nxp_sdma_channels_atomic_##inst,                                  \
			     FSL_FEATURE_SDMA_MODULE_CHANNEL);                                     \
	static struct sdma_dev_data sdma_data_##inst = {                                           \
		.channels_atomic = dma_nxp_sdma_channels_atomic_##inst,                            \
	};                                                                                         \
	static void dma_nxp_sdma_##inst_irq_config(void);                                          \
	static const struct sdma_dev_cfg sdma_cfg_##inst = {                                       \
		.base = (SDMAARM_Type *)DT_INST_REG_ADDR(inst),                                    \
		.irq_config = dma_nxp_sdma_##inst_irq_config,                                      \
		.clk_dev = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, clocks),	\
				       (DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst))),	\
				       (NULL)),                        \
			 .bus_clk_id = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, clocks),	\
					  (DT_INST_CLOCKS_CELL(inst, name)),	\
					  (0)),                            \
	};                                                                                         \
	static void dma_nxp_sdma_##inst_irq_config(void)                                           \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), dma_nxp_sdma_isr,     \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}                                                                                          \
	PM_DEVICE_DT_INST_DEFINE(inst, sdma_pm_action);                                            \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, dma_nxp_sdma_init, PM_DEVICE_DT_INST_GET(inst),                \
			      &sdma_data_##inst, &sdma_cfg_##inst, PRE_KERNEL_1,                   \
			      CONFIG_DMA_INIT_PRIORITY, &sdma_api);

DT_INST_FOREACH_STATUS_OKAY(DMA_NXP_SDMA_INIT);
