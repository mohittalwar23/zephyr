/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/mpipe/mpipe_src.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(mpipe_src_api, NULL, NULL, NULL, NULL, NULL);

static int activate_calls;
static int deactivate_calls;
static int activate_result;
static int deactivate_result;
static bool activate_saw_open_gate;
static bool deactivate_saw_closed_gate;

static int test_activate(struct mpipe_src *src)
{
	activate_calls++;
	activate_saw_open_gate = mpipe_src_delivery_enter(src);
	if (activate_saw_open_gate) {
		mpipe_src_delivery_leave(src);
	}

	return activate_result;
}

static int test_deactivate(struct mpipe_src *src)
{
	deactivate_calls++;
	deactivate_saw_closed_gate = !mpipe_src_delivery_enter(src);

	return deactivate_result;
}

static void init_push_source(struct mpipe_src *src)
{
	memset(src, 0, sizeof(*src));
	zassert_ok(mpipe_src_init(src, 1U));
	src->drive = MPIPE_SRC_DRIVE_PUSH;
	src->activate = test_activate;
	src->deactivate = test_deactivate;

	activate_calls = 0;
	deactivate_calls = 0;
	activate_result = 0;
	deactivate_result = 0;
	activate_saw_open_gate = false;
	deactivate_saw_closed_gate = false;
}

/* Initialization is a constructor, not a request for callers to zero storage. */
ZTEST(mpipe_src_api, test_init_overwrites_nonzero_scheduling_state)
{
	struct mpipe_src src;

	memset(&src, 0xa5, sizeof(src));
	zassert_ok(mpipe_src_init(&src, 1U));

	zassert_equal(src.drive, MPIPE_SRC_DRIVE_PULL);
	zassert_is_null(src.pool);
	zassert_equal(src.num_buffers, 0U);
	zassert_is_null(src.decide_buffer_pool);
}

ZTEST(mpipe_src_api, test_push_source_is_active_only_while_playing)
{
	struct mpipe_src src;

	init_push_source(&src);
	zassert_false(mpipe_src_delivery_enter(&src), "source admitted before PLAYING");

	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
		      0);
	zassert_equal(activate_calls, 1);
	zassert_true(activate_saw_open_gate, "activate ran before admission opened");
	zassert_true(mpipe_src_delivery_enter(&src), "PLAYING refused a delivery");
	mpipe_src_delivery_leave(&src);

	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED),
		      0);
	zassert_equal(deactivate_calls, 1);
	zassert_true(deactivate_saw_closed_gate, "deactivate ran while admission was open");
	zassert_false(mpipe_src_delivery_enter(&src), "PAUSED admitted a delivery");
}

ZTEST(mpipe_src_api, test_activation_failure_recloses_admission)
{
	struct mpipe_src src;

	init_push_source(&src);
	activate_result = -EIO;

	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
		      -EIO);
	zassert_equal(activate_calls, 1);
	zassert_false(mpipe_src_delivery_enter(&src),
		      "failed activation left admission open");
}

ZTEST(mpipe_src_api, test_deactivation_failure_keeps_admission_closed)
{
	struct mpipe_src src;

	init_push_source(&src);
	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
		      0);
	deactivate_result = -EIO;

	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED),
		      -EIO);
	zassert_equal(deactivate_calls, 1);
	zassert_true(deactivate_saw_closed_gate);
	zassert_false(mpipe_src_delivery_enter(&src),
		      "failed deactivation reopened admission");
}

ZTEST(mpipe_src_api, test_pull_source_does_not_run_push_lifecycle_hooks)
{
	struct mpipe_src src;

	init_push_source(&src);
	src.drive = MPIPE_SRC_DRIVE_PULL;

	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
		      0);
	zassert_equal(mpipe_src_change_state(&src.element,
				     MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED),
		      0);
	zassert_equal(activate_calls, 0);
	zassert_equal(deactivate_calls, 0);
	zassert_false(mpipe_src_delivery_enter(&src));
}

#define TEST_THREAD_STACK_SIZE 1024

K_THREAD_STACK_DEFINE(delivery_stack, TEST_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(deactivate_stack, TEST_THREAD_STACK_SIZE);

static struct mpipe_src race_src;
static struct k_thread delivery_thread;
static struct k_thread deactivate_thread;
static struct k_sem delivery_entered;
static struct k_sem release_delivery;
static struct k_sem deactivate_started;
static struct k_sem deactivate_done;
static int race_deactivate_result;

static void delivery_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!mpipe_src_delivery_enter(&race_src)) {
		return;
	}

	k_sem_give(&delivery_entered);
	k_sem_take(&release_delivery, K_FOREVER);
	mpipe_src_delivery_leave(&race_src);
}

static void deactivate_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_give(&deactivate_started);
	race_deactivate_result = mpipe_src_change_state(
		&race_src.element, MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED);
	k_sem_give(&deactivate_done);
}

ZTEST(mpipe_src_api, test_deactivation_rejects_new_delivery_and_drains_accepted_one)
{
	bool gate_closed = false;

	init_push_source(&race_src);
	k_sem_init(&delivery_entered, 0, 1);
	k_sem_init(&release_delivery, 0, 1);
	k_sem_init(&deactivate_started, 0, 1);
	k_sem_init(&deactivate_done, 0, 1);
	zassert_equal(mpipe_src_change_state(&race_src.element,
				     MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
		      0);

	k_thread_create(&delivery_thread, delivery_stack,
			K_THREAD_STACK_SIZEOF(delivery_stack), delivery_worker,
			NULL, NULL, NULL, CONFIG_ZTEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&delivery_entered, K_SECONDS(1)));

	k_thread_create(&deactivate_thread, deactivate_stack,
			K_THREAD_STACK_SIZEOF(deactivate_stack), deactivate_worker,
			NULL, NULL, NULL, CONFIG_ZTEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&deactivate_started, K_SECONDS(1)));

	for (int i = 0; i < 1000; i++) {
		if (!mpipe_src_delivery_enter(&race_src)) {
			gate_closed = true;
			break;
		}
		mpipe_src_delivery_leave(&race_src);
		k_yield();
	}

	zassert_true(gate_closed, "deactivation never closed admission");
	zassert_equal(k_sem_take(&deactivate_done, K_NO_WAIT), -EBUSY,
		      "deactivation returned before the accepted delivery left");

	k_sem_give(&release_delivery);
	zassert_ok(k_sem_take(&deactivate_done, K_SECONDS(1)));
	zassert_equal(race_deactivate_result, 0);
	zassert_true(deactivate_saw_closed_gate);
	zassert_ok(k_thread_join(&delivery_thread, K_SECONDS(1)));
	zassert_ok(k_thread_join(&deactivate_thread, K_SECONDS(1)));
}
