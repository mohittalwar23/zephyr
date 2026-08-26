/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#include <dma_nxp_sdma_irq.h>

struct fake_sdma_ring {
	bool owned[DMA_NXP_SDMA_BD_COUNT];
	uint32_t rearm_order[DMA_NXP_SDMA_BD_COUNT];
	uint32_t rearm_count;
};

struct fake_sdma_irq {
	struct dma_nxp_sdma_descriptor_state descriptors;
	struct dma_nxp_sdma_irq_state notifications;
	struct fake_sdma_ring ring;
	uint32_t current_bd;
	uint32_t ccb_current_bd;
	uint32_t callback_count;
	int callback_status[DMA_NXP_SDMA_BD_COUNT + 1U];
	uint32_t restart_count;
	uint32_t stop_count;
	bool started;
	bool stop_on_first_callback;
};

static bool fake_sdma_owned(void *context, uint32_t index)
{
	struct fake_sdma_irq *fake = context;

	return fake->ring.owned[index];
}

static void fake_sdma_rearm(void *context, uint32_t index, uint32_t size)
{
	struct fake_sdma_irq *fake = context;

	ARG_UNUSED(size);
	fake->ring.rearm_order[fake->ring.rearm_count++] = index;
	fake->ring.owned[index] = true;
}

/* This is the cursor/CCB part of pinned SDMA_HandleIRQ(), without its callback. */
static uint32_t fake_sdma_advance_irq(void *context)
{
	struct fake_sdma_irq *fake = context;

	fake->current_bd = (fake->current_bd + 1U) % fake->descriptors.bd_count;
	fake->ccb_current_bd = fake->current_bd;

	return fake->current_bd;
}

static void fake_sdma_stop_hardware(void *context)
{
	struct fake_sdma_irq *fake = context;

	fake->stop_count++;
}

static void fake_sdma_user_callback(struct fake_sdma_irq *fake, int status)
{
	fake->callback_status[fake->callback_count++] = status;
	if (fake->stop_on_first_callback && fake->callback_count == 1U) {
		fake->started = false;
		dma_nxp_sdma_irq_stop(fake_sdma_stop_hardware, fake);
	}
}

static void fake_sdma_driver_callback(struct fake_sdma_irq *fake, uint32_t bd_index)
{
	uint32_t completed = 0U;
	int status;
	int ret;

	if (fake->started) {
		ret = dma_nxp_sdma_irq_complete_descriptors(
			&fake->notifications, &fake->descriptors, MEMORY_TO_PERIPHERAL,
			fake_sdma_owned, fake_sdma_rearm, fake, bd_index,
			fake_sdma_advance_irq, fake, &completed);
		if (ret != 0) {
			fake->started = false;
			fake_sdma_stop_hardware(fake);
		} else if (completed != 0U) {
			fake->restart_count++;
		}
	}

	while (dma_nxp_sdma_irq_take(&fake->notifications, false, &status)) {
		fake_sdma_user_callback(fake, status);
	}
}

/* Pinned SDMA_HandleIRQ() advances the HAL/CCB cursor before the driver callback. */
static void fake_sdma_handle_irq(struct fake_sdma_irq *fake)
{
	uint32_t bd_index = fake_sdma_advance_irq(fake);

	fake_sdma_driver_callback(fake, bd_index);
}

static void fake_sdma_init(struct fake_sdma_irq *fake)
{
	struct dma_block_config blocks[] = {
		{
			.source_address = 0x1000U,
			.dest_address = 0x2000U,
			.block_size = 16U,
		},
		{
			.source_address = 0x3000U,
			.dest_address = 0x4000U,
			.block_size = 24U,
		},
	};
	struct dma_config config = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 4U,
		.dest_data_size = 4U,
		.source_burst_length = 1U,
		.dest_burst_length = 1U,
		.block_count = ARRAY_SIZE(blocks),
		.head_block = &blocks[0],
	};

	memset(fake, 0, sizeof(*fake));
	blocks[0].next_block = &blocks[1];
	for (size_t i = 0U; i < ARRAY_SIZE(fake->ring.owned); i++) {
		fake->ring.owned[i] = true;
	}
	zassert_ok(dma_nxp_sdma_descriptor_prepare(&fake->descriptors, &config));
	zassert_ok(dma_nxp_sdma_descriptor_init_stat(&fake->descriptors,
						     MEMORY_TO_PERIPHERAL));
	dma_nxp_sdma_irq_init(&fake->notifications);
	fake->started = true;
}

ZTEST(dma_reload, test_sdma_fake_irq_delivers_coalesced_callbacks_once)
{
	struct fake_sdma_irq fake;

	fake_sdma_init(&fake);
	fake.ring.owned[0] = false;
	fake.ring.owned[1] = false;

	fake_sdma_handle_irq(&fake);

	zassert_equal(fake.callback_count, 2U, "coalesced IRQ delivered %u callbacks",
		      fake.callback_count);
	zassert_equal(fake.callback_status[0], DMA_STATUS_BLOCK);
	zassert_equal(fake.callback_status[1], DMA_STATUS_BLOCK);
	zassert_equal(fake.ring.rearm_order[0], 0U, "first callback was out of order");
	zassert_equal(fake.ring.rearm_order[1], 1U, "second callback was out of order");
	zassert_equal(fake.current_bd, 0U, "HAL next-BD cursor was not reconciled");
	zassert_equal(fake.ccb_current_bd, 0U, "CCB next-BD cursor was not reconciled");
	zassert_equal(fake.descriptors.stat.total_copied, 40U);

	fake_sdma_handle_irq(&fake);
	zassert_equal(fake.callback_count, 2U, "empty IRQ duplicated a callback");
	zassert_equal(fake.current_bd, 0U, "empty IRQ moved the HAL next-BD cursor");
	zassert_equal(fake.ccb_current_bd, 0U, "empty IRQ moved the CCB next-BD cursor");
}

ZTEST(dma_reload, test_sdma_stop_from_first_callback_preserves_committed_success)
{
	struct fake_sdma_irq fake;

	fake_sdma_init(&fake);
	fake.stop_on_first_callback = true;
	fake.ring.owned[0] = false;
	fake.ring.owned[1] = false;

	fake_sdma_handle_irq(&fake);

	zassert_equal(fake.callback_count, 2U, "stop discarded a committed success callback");
	zassert_equal(fake.callback_status[0], DMA_STATUS_BLOCK);
	zassert_equal(fake.callback_status[1], DMA_STATUS_BLOCK);
	zassert_equal(fake.restart_count, 1U, "completed IRQ did not perform its one restart");
	zassert_equal(fake.stop_count, 1U, "callback stop did not stop hardware exactly once");
	zassert_false(fake.started, "callback stop did not remain committed");
}

ZTEST(dma_reload, test_sdma_stop_preserves_committed_terminal_error)
{
	struct fake_sdma_irq fake;

	fake_sdma_init(&fake);
	fake.stop_on_first_callback = true;
	/* Only the first completed descriptor has matching software credit. */
	fake.descriptors.stat.write_position = 16U;
	fake.descriptors.stat.pending_length = 16U;
	fake.descriptors.stat.free = fake.descriptors.capacity - 16U;
	fake.ring.owned[0] = false;
	fake.ring.owned[1] = false;

	fake_sdma_handle_irq(&fake);

	zassert_equal(fake.callback_count, 2U, "stop discarded the committed terminal error");
	zassert_equal(fake.callback_status[0], DMA_STATUS_BLOCK);
	zassert_equal(fake.callback_status[1], -EINVAL);
	zassert_equal(fake.restart_count, 0U, "failed IRQ restarted hardware");
	zassert_equal(fake.stop_count, 2U,
		      "error stop and callback stop did not each reach hardware");
	zassert_false(fake.started, "terminal error did not stop the channel");
}
