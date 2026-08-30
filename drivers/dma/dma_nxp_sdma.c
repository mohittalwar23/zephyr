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
#include "fsl_sdma.h"
#include "dma_nxp_sdma_accounting.h"
#include "dma_nxp_sdma_append.h"
#include "dma_nxp_sdma_common.h"
#include "dma_nxp_sdma_irq.h"
#include "dma_nxp_sdma_lifecycle.h"

LOG_MODULE_REGISTER(nxp_sdma);

#define DMA_NXP_SDMA_CHAN_DEFAULT_PRIO 4

#define DT_DRV_COMPAT nxp_sdma

BUILD_ASSERT(kSDMA_PeripheralNormal_SP == DMA_NXP_SDMA_PERIPHERAL_NORMAL_SP);
BUILD_ASSERT(kSDMA_PeripheralMultiFifoPDM == DMA_NXP_SDMA_PERIPHERAL_MULTI_FIFO_PDM);
BUILD_ASSERT((kSDMA_PeripheralNormal_SP & DMA_NXP_SDMA_MODE_APPEND) == 0U);
BUILD_ASSERT((kSDMA_PeripheralMultiFifoPDM & DMA_NXP_SDMA_MODE_APPEND) == 0U);

static struct dma_nxp_sdma_ram_script_state ram_script_state;

struct sdma_dev_cfg {
	SDMAARM_Type *base;
	void (*irq_config)(void);
	uint32_t event_count;
	struct dma_nxp_sdma_context_store contexts;
};

struct sdma_channel_data {
	sdma_handle_t handle;
	sdma_transfer_config_t transfer_cfg;
	sdma_peripheral_t peripheral;
	uint32_t direction;
	struct dma_nxp_sdma_request_state request;
	const struct device *dev;
	sdma_buffer_descriptor_t *bd_pool; /*pre-allocated list of BD used for transfer */
	struct dma_nxp_sdma_descriptor_state descriptor_state;
	struct dma_config *dma_cfg;
	struct dma_nxp_sdma_lifecycle lifecycle;
	struct dma_nxp_sdma_irq_state irq;
	bool error_callback_dis;
	bool append_mode;
	bool ram_script_claimed;
	uint32_t bus_width;
	struct dma_nxp_sdma_append_state append;

	void *arg; /* argument passed to user-defined DMA callback */
	dma_callback_t cb; /* user-defined callback for DMA transfer completion */
};

struct dma_nxp_sdma_completion {
	struct sdma_channel_data *chan_data;
	uint32_t bd_index;
};

struct sdma_dev_data {
	struct dma_context dma_ctx;
	atomic_t *channels_atomic;
	struct sdma_channel_data chan[FSL_FEATURE_SDMA_MODULE_CHANNEL];
	sdma_buffer_descriptor_t bd_pool[FSL_FEATURE_SDMA_MODULE_CHANNEL]
					[CONFIG_DMA_NXP_SDMA_BD_COUNT] __aligned(64);
	struct k_mutex ch0_lock; /* serialises the shared channel-0 context load */
};

/*
 * Drains one queued notification per call: successful block completions
 * first (one per completed descriptor, in order), then at most one final
 * error. Callers loop this until it returns false.
 */
static bool dma_nxp_sdma_take_callback(struct sdma_channel_data *chan_data, int *status)
{
	k_spinlock_key_t key = k_spin_lock(&chan_data->lifecycle.lock);
	bool notify = false;

	notify = dma_nxp_sdma_irq_take(&chan_data->irq, chan_data->error_callback_dis, status);
	k_spin_unlock(&chan_data->lifecycle.lock, key);

	return notify;
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
			int status;

			chan_data = &dev_data->chan[i];
			SDMA_ClearChannelInterruptStatus(dev_cfg->base, 1 << i);
			SDMA_HandleIRQ(&chan_data->handle);

			while (dma_nxp_sdma_take_callback(chan_data, &status)) {
				if (chan_data->cb) {
					chan_data->cb(chan_data->dev, chan_data->arg, i, status);
				}
			}
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

static int sdma_set_peripheral_type(struct dma_config *config, sdma_peripheral_t *type,
				    bool *append_mode)
{
	uint32_t peripheral;
	int ret;

	ret = dma_nxp_sdma_validate_slot(config, &peripheral, append_mode);
	if (ret < 0) {
		return ret;
	}
	*type = (sdma_peripheral_t)peripheral;

	return 0;
}

static void dma_nxp_sdma_start_hardware(void *context)
{
	struct sdma_channel_data *chan_data = context;
	const struct sdma_dev_cfg *dev_cfg = chan_data->dev->config;

	SDMA_SetChannelPriority(dev_cfg->base, chan_data->request.channel,
				DMA_NXP_SDMA_CHAN_DEFAULT_PRIO);
	SDMA_StartTransfer(&chan_data->handle);
}

static void dma_nxp_sdma_stop_transfer(void *context)
{
	struct sdma_channel_data *chan_data = context;

	SDMA_StopTransfer(&chan_data->handle);
}

static void dma_nxp_sdma_stop_hardware(void *context)
{
	struct sdma_channel_data *chan_data = context;

	dma_nxp_sdma_irq_stop(dma_nxp_sdma_stop_transfer, chan_data);
}

static void dma_nxp_sdma_stop_after_error(struct sdma_channel_data *chan_data)
{
	SDMA_StopTransfer(&chan_data->handle);
}

static void dma_nxp_sdma_rearm_descriptor(void *context, uint32_t index, uint32_t size)
{
	struct sdma_channel_data *chan_data = context;
	sdma_buffer_descriptor_t *bd = &chan_data->bd_pool[index];

	bd->count = size;
	bd->status |= (uint8_t)kSDMA_BDStatusDone;
}

/* Done cleared means the SDMA engine has released the BD to software. */
static bool dma_nxp_sdma_bd_owned(void *context, uint32_t index)
{
	struct sdma_channel_data *chan_data = context;

	return (chan_data->bd_pool[index].status & (uint8_t)kSDMA_BDStatusDone) != 0U;
}

static uint32_t dma_nxp_sdma_advance_irq(void *context)
{
	sdma_handle_t *handle = context;
	sdma_callback callback = handle->callback;

	handle->callback = NULL;
	SDMA_HandleIRQ(handle);
	handle->callback = callback;

	return handle->bdIndex;
}

static bool dma_nxp_sdma_complete(void *context)
{
	struct dma_nxp_sdma_completion *completion = context;
	struct sdma_channel_data *chan_data = completion->chan_data;
	uint32_t count = 0U;
	uint32_t next_bd;
	bool restart;
	int ret;

	if (chan_data->append_mode) {
		ret = dma_nxp_sdma_append_complete(&chan_data->append, dma_nxp_sdma_bd_owned,
						   chan_data, &count);
		next_bd = (chan_data->append.write_index + chan_data->handle.bdCount -
			   chan_data->append.pending_count) % chan_data->handle.bdCount;
		ret = dma_nxp_sdma_irq_finalize(&chan_data->irq, count, ret,
						completion->bd_index, next_bd,
						chan_data->handle.bdCount,
						dma_nxp_sdma_advance_irq, &chan_data->handle);
		restart = (ret == 0) && chan_data->append.pending_count != 0U;
	} else {
		ret = dma_nxp_sdma_irq_complete_descriptors(
			&chan_data->irq, &chan_data->descriptor_state, chan_data->direction,
			dma_nxp_sdma_bd_owned, dma_nxp_sdma_rearm_descriptor, chan_data,
			completion->bd_index, dma_nxp_sdma_advance_irq, &chan_data->handle, &count);
		restart = (ret == 0) && count != 0U;
	}

	if (ret != 0) {
		chan_data->lifecycle.started = false;
		dma_nxp_sdma_stop_after_error(chan_data);
		return false;
	}

	return restart;
}

void dma_nxp_sdma_callback(sdma_handle_t *handle, void *userData, bool TransferDone,
			   uint32_t bdIndex)
{
	struct sdma_channel_data *chan_data = userData;
	struct dma_nxp_sdma_completion completion = {
		.chan_data = chan_data,
		.bd_index = bdIndex,
	};

	ARG_UNUSED(handle);
	ARG_UNUSED(TransferDone);
	dma_nxp_sdma_lifecycle_complete(&chan_data->lifecycle, dma_nxp_sdma_complete, &completion,
					 dma_nxp_sdma_start_hardware, chan_data);
}

static int dma_nxp_sdma_channel_init(const struct device *dev, uint32_t channel)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;

	chan_data = &dev_data->chan[channel];
	SDMA_CreateHandle(&chan_data->handle, dev_cfg->base, channel,
			  dma_nxp_sdma_context_at(&dev_cfg->contexts, channel));

	SDMA_SetCallback(&chan_data->handle, dma_nxp_sdma_callback, chan_data);

	return 0;
}

static void dma_nxp_sdma_setup_bd(const struct device *dev, uint32_t channel,
				  const struct dma_config *config)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	sdma_buffer_descriptor_t *crt_bd;
	uint32_t bd_count;
	uint32_t i;

	chan_data = &dev_data->chan[channel];

	chan_data->bd_pool = &dev_data->bd_pool[channel][0];
	bd_count = chan_data->append_mode ? CONFIG_DMA_NXP_SDMA_BD_COUNT
					  : chan_data->descriptor_state.bd_count;
	if (!chan_data->append_mode) {
		memset(&chan_data->append, 0, sizeof(chan_data->append));
	}

	memset(chan_data->bd_pool, 0, sizeof(sdma_buffer_descriptor_t) * bd_count);
	SDMA_InstallBDMemory(&chan_data->handle, chan_data->bd_pool, bd_count);

	crt_bd = chan_data->bd_pool;
	for (i = 0U; i < chan_data->descriptor_state.bd_count; i++) {
		bool is_last = false;
		bool is_wrap = false;

		if (chan_data->append_mode) {
			is_last = true;
			is_wrap = i == bd_count - 1U;
		} else if (i == chan_data->descriptor_state.bd_count - 1U) {
			is_last = true;
			is_wrap = true;
		}

		SDMA_ConfigBufferDescriptor(crt_bd,
			chan_data->descriptor_state.source_address[i],
			chan_data->descriptor_state.dest_address[i],
			chan_data->bus_width, chan_data->descriptor_state.bd_size[i],
			is_last, true, is_wrap, chan_data->transfer_cfg.type);
		crt_bd++;
	}

	if (chan_data->append_mode) {
		chan_data->bd_pool[bd_count - 1U].status |= (uint8_t)kSDMA_BDStatusWrap;
	}
}

static int dma_nxp_sdma_config(const struct device *dev, uint32_t channel,
			       struct dma_config *config)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	struct dma_nxp_sdma_descriptor_state descriptor_state;
	struct dma_nxp_sdma_append_state append_state = {0};
	sdma_peripheral_t peripheral;
	k_spinlock_key_t key;
	bool append_mode;
	bool ram_script_required;
	uint32_t dest_width;
	uint32_t source_width;
	uint32_t watermark;
	int ret;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL || config == NULL) {
		LOG_ERR("sdma_config() invalid channel %d", channel);
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];
	ret = dma_nxp_sdma_request_validate(&chan_data->request, channel);
	if (ret < 0) {
		LOG_ERR("%s: channel %u has no SDMA event request", __func__, channel);
		return ret;
	}

	ret = dma_nxp_sdma_encode_width(config->source_data_size, &source_width);
	if (ret < 0) {
		LOG_ERR("%s: unsupported source width %u", __func__, config->source_data_size);
		return ret;
	}
	ret = dma_nxp_sdma_encode_width(config->dest_data_size, &dest_width);
	if (ret < 0) {
		LOG_ERR("%s: unsupported destination width %u", __func__, config->dest_data_size);
		return ret;
	}

	ret = dma_nxp_sdma_descriptor_prepare(&descriptor_state, config);
	if (ret < 0) {
		return ret;
	}
	ret = sdma_set_peripheral_type(config, &peripheral, &append_mode);
	if (ret < 0) {
		LOG_ERR("%s: failed to set peripheral type", __func__);
		return ret;
	}
	if (append_mode && config->block_count >= CONFIG_DMA_NXP_SDMA_BD_COUNT) {
		LOG_ERR("%s: append block_count %u leaves no free BD in pool %u", __func__,
			config->block_count, CONFIG_DMA_NXP_SDMA_BD_COUNT);
		return -EINVAL;
	}
	if (append_mode) {
		ret = dma_nxp_sdma_append_prepare(&append_state, &descriptor_state);
		if (ret < 0) {
			return ret;
		}
	}
	dma_nxp_sdma_channel_init(dev, channel);

	chan_data->request.channel = channel;

	key = k_spin_lock(&chan_data->lifecycle.lock);
	chan_data->lifecycle.started = false;
	dma_nxp_sdma_irq_init(&chan_data->irq);
	chan_data->error_callback_dis = config->error_callback_dis;
	k_spin_unlock(&chan_data->lifecycle.lock, key);
	chan_data->dev = dev;
	chan_data->direction = config->channel_direction;
	chan_data->descriptor_state = descriptor_state;
	chan_data->append_mode = append_mode;
	chan_data->append = append_state;
	chan_data->bus_width = config->channel_direction == MEMORY_TO_PERIPHERAL
				       ? dest_width
				       : source_width;

	chan_data->cb = config->dma_callback;
	chan_data->arg = config->user_data;

	sdma_set_transfer_type(config, &chan_data->transfer_cfg.type);

	chan_data->peripheral = peripheral;

	if (chan_data->peripheral == kSDMA_PeripheralMultiFifoPDM) {
		unsigned int n_fifos = 4; /* TODO: make this configurable */

		SDMA_SetMultiFifoConfig(&chan_data->transfer_cfg, n_fifos, 0);
		SDMA_EnableSwDone(dev_cfg->base, &chan_data->transfer_cfg, 0,
				  chan_data->peripheral);
	}

	dma_nxp_sdma_setup_bd(dev, channel, config);
	ret = dma_nxp_sdma_descriptor_init_stat(&chan_data->descriptor_state,
						 chan_data->direction);
	if (ret < 0) {
		LOG_ERR("%s: failed to init stat", __func__);
		return ret;
	}

	ram_script_required = SDMA_DRIVER_LOAD_RAM_SCRIPT &&
			      peripheral == kSDMA_PeripheralMultiFifoPDM;
	ret = dma_nxp_sdma_ram_script_claim(&ram_script_state, dev, ram_script_required,
					    &chan_data->ram_script_claimed);
	if (ret < 0) {
		LOG_ERR("%s: RAM-script peripheral already belongs to another controller",
			__func__);
		return ret;
	}
	if (!ram_script_required) {
		dma_nxp_sdma_ram_script_release(&ram_script_state, dev,
						&chan_data->ram_script_claimed);
	}

	/*
	 * The watermark is how many bytes the engine moves per peripheral DMA
	 * request; it must match the consumer's burst length (e.g. the SAI word
	 * size), not a fixed value, or the channel waits for data that never
	 * arrives.
	 */
	watermark = chan_data->direction == PERIPHERAL_TO_MEMORY ? config->source_burst_length
								 : config->dest_burst_length;
	if (watermark == 0) {
		watermark = 64;
	}

	/* prepare first block for transfer ...*/
	SDMA_PrepareTransfer(&chan_data->transfer_cfg,
			     chan_data->descriptor_state.source_address[0],
			     chan_data->descriptor_state.dest_address[0],
			     config->source_data_size, config->dest_data_size, watermark,
			     chan_data->descriptor_state.bd_size[0],
			     chan_data->request.event_source,
			     chan_data->peripheral, chan_data->transfer_cfg.type);

	/*... and submit it to SDMA engine.
	 * Note that SDMA transfer is later manually started by the dma_nxp_sdma_start()
	 */
	/*
	 * SDMA_SubmitTransfer() loads the context through the single shared
	 * channel-0 boot script; concurrent config() calls (e.g. TX and RX)
	 * would corrupt each other's load. config() is always thread context,
	 * so a mutex is safe.
	 */
	k_mutex_lock(&dev_data->ch0_lock, K_FOREVER);
	SDMA_SubmitTransfer(&chan_data->handle, &chan_data->transfer_cfg);
	k_mutex_unlock(&dev_data->ch0_lock);

	return 0;
}

static int dma_nxp_sdma_start(const struct device *dev, uint32_t channel)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		LOG_ERR("%s: invalid channel %d", __func__, channel);
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];

	dma_nxp_sdma_lifecycle_start(&chan_data->lifecycle, dma_nxp_sdma_start_hardware,
				     chan_data);

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

	dma_nxp_sdma_lifecycle_stop(&chan_data->lifecycle, dma_nxp_sdma_stop_hardware,
				    chan_data);
	return 0;
}

static int dma_nxp_sdma_get_status(const struct device *dev, uint32_t channel,
				   struct dma_status *stat)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	k_spinlock_key_t key;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL || stat == NULL) {
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];

	key = k_spin_lock(&chan_data->lifecycle.lock);
	if (chan_data->append_mode) {
		dma_nxp_sdma_append_status(&chan_data->append, chan_data->lifecycle.started,
					   chan_data->direction, stat);
	} else {
		stat->busy = chan_data->lifecycle.started;
		stat->dir = chan_data->direction;
		stat->free = chan_data->descriptor_state.stat.free;
		stat->pending_length = chan_data->descriptor_state.stat.pending_length;
		stat->read_position = chan_data->descriptor_state.stat.read_position;
		stat->write_position = chan_data->descriptor_state.stat.write_position;
		stat->total_copied = chan_data->descriptor_state.stat.total_copied;
	}
	k_spin_unlock(&chan_data->lifecycle.lock, key);

	return 0;
}

static int dma_nxp_sdma_reload(const struct device *dev, uint32_t channel, uint32_t src,
			       uint32_t dst, size_t size)
{
	struct sdma_dev_data *dev_data = dev->data;
	struct sdma_channel_data *chan_data;
	k_spinlock_key_t key;
	int ret;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		return -EINVAL;
	}

	chan_data = &dev_data->chan[channel];

	if (!size) {
		return 0;
	}
	key = k_spin_lock(&chan_data->lifecycle.lock);
	if (chan_data->append_mode) {
		sdma_buffer_descriptor_t *bd;
		struct dma_nxp_sdma_append_slot slot;
		bool restart;

		ret = dma_nxp_sdma_append_reload(&chan_data->append,
						 chan_data->lifecycle.started, size, &slot,
						 &restart);
		if (ret < 0) {
			goto out;
		}

		bd = &chan_data->bd_pool[slot.index];
		SDMA_ConfigBufferDescriptor(bd, src, dst, chan_data->bus_width, size,
					    slot.last, true, slot.wrap,
					    chan_data->transfer_cfg.type);
		if (restart) {
			dma_nxp_sdma_start_hardware(chan_data);
		}
		ret = 0;
		goto out;
	}

	ret = dma_nxp_sdma_descriptor_reload(&chan_data->descriptor_state,
					     chan_data->direction, src, dst, size);

out:
	k_spin_unlock(&chan_data->lifecycle.lock, key);

	return ret;
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
		*val = CONFIG_DMA_NXP_SDMA_BD_COUNT;
		break;
	default:
		LOG_ERR("invalid attribute type: %d", type);
		return -EINVAL;
	}
	return 0;
}

static bool sdma_channel_filter(const struct device *dev, int chan_id, void *param)
{
	const struct sdma_dev_cfg *dev_cfg = dev->config;
	struct sdma_dev_data *dev_data = dev->data;
	struct dma_nxp_sdma_request_state request;

	if (!dma_nxp_sdma_request_admit(&request, chan_id, param,
					FSL_FEATURE_SDMA_MODULE_CHANNEL,
					dev_cfg->event_count)) {
		return false;
	}

	dev_data->chan[chan_id].request = request;
	dev_data->chan[chan_id].descriptor_state.capacity = 0;

	return true;
}

static void sdma_channel_release(const struct device *dev, uint32_t channel)
{
	struct sdma_dev_data *dev_data = dev->data;

	if (channel >= FSL_FEATURE_SDMA_MODULE_CHANNEL) {
		return;
	}

	dma_nxp_sdma_ram_script_release(&ram_script_state, dev,
					&dev_data->chan[channel].ram_script_claimed);
	dma_nxp_sdma_request_release(&dev_data->chan[channel].request);
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

	SDMA_Init(cfg->base, &defconfig);

	k_mutex_init(&data->ch0_lock);

	/* configure interrupts */
	cfg->irq_config();

	return 0;
}

#define DMA_NXP_SDMA_INIT(inst)						\
	BUILD_ASSERT(DT_INST_PROP(inst, dma_requests) > 0 &&		\
		     DT_INST_PROP(inst, dma_requests) <= FSL_FEATURE_SDMA_EVENT_NUM, \
		     "dma-requests exceeds the MCUX SDMA event table");	\
	static ATOMIC_DEFINE(dma_nxp_sdma_channels_atomic_##inst,	\
			     FSL_FEATURE_SDMA_MODULE_CHANNEL);		\
	static AT_NONCACHEABLE_SECTION_ALIGN(sdma_context_data_t	\
		sdma_contexts_##inst[FSL_FEATURE_SDMA_MODULE_CHANNEL], 4); \
	static struct sdma_dev_data sdma_data_##inst = {		\
		.channels_atomic = dma_nxp_sdma_channels_atomic_##inst,	\
	};								\
	static void dma_nxp_sdma_##inst##_irq_config(void);		\
	static const struct sdma_dev_cfg sdma_cfg_##inst = {		\
		.base = (SDMAARM_Type *)DT_INST_REG_ADDR(inst),				\
		.irq_config = dma_nxp_sdma_##inst##_irq_config,		\
		.event_count = DT_INST_PROP(inst, dma_requests),		\
		.contexts = {						\
			.base = sdma_contexts_##inst,			\
			.stride = sizeof(sdma_contexts_##inst[0]),	\
			.count = ARRAY_SIZE(sdma_contexts_##inst),	\
		},							\
	};								\
	static void dma_nxp_sdma_##inst##_irq_config(void)		\
	{								\
		IRQ_CONNECT(DT_INST_IRQN(inst),				\
			    DT_INST_IRQ(inst, priority),		\
			    dma_nxp_sdma_isr, DEVICE_DT_INST_GET(inst), 0);	\
		irq_enable(DT_INST_IRQN(inst));				\
	}								\
	DEVICE_DT_INST_DEFINE(inst, dma_nxp_sdma_init, NULL,		\
			      &sdma_data_##inst, &sdma_cfg_##inst,	\
			      PRE_KERNEL_1, CONFIG_DMA_INIT_PRIORITY,	\
			      &sdma_api);				\

DT_INST_FOREACH_STATUS_OKAY(DMA_NXP_SDMA_INIT);
