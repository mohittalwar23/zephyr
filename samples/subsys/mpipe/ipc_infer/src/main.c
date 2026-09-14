/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The end-to-end path, minus the microphone: the M7 streams 16 kHz mono audio
 * into the shared ring, the HiFi4 consumes it, runs micro_speech over each
 * second, and reports what it heard back over the control link.
 *
 * Both images are the same source. The role, the mailbox and the shared window
 * all come from devicetree; what differs is that only the DSP builds inference.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_protocol.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
#include "consumer.h"
#endif

LOG_MODULE_REGISTER(mpipe_ipc_infer, LOG_LEVEL_INF);

#include <fsl_mu.h>

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
/* The C surface of the C++ inference code. */
void model_runner_init(void);
int micro_speech_process_audio(const int16_t *audio_data, size_t audio_data_size);
const char *micro_speech_category_label(int category);


static void signal_linux_ready(void)
{
	MU_Type *const mu2_b = (MU_Type *)(uintptr_t)0x30e70000U;

	(void)MU_TriggerInterrupts(mu2_b, kMU_GenInt0InterruptTrigger);
}
#else
static inline void signal_linux_ready(void) { }

#include "producer.h"
#endif

#ifndef CONFIG_SOC_MIMX8ML8_ADSP
/* Mirrors kCategoryLabels in the model settings the DSP builds against. */
static const char *category_label(uint32_t category)
{
	static const char *const labels[] = { "silence", "unknown", "yes", "no" };

	return (category < ARRAY_SIZE(labels)) ? labels[category] : "?";
}
#endif

#define IPC_NODE    DT_NODELABEL(ipc0)
#define SHARED_NODE DT_NODELABEL(mpipe_ipc_ctrl)

#define IS_HOST (DT_ENUM_IDX_OR(IPC_NODE, role, 0) == 0)

static volatile struct mpipe_ipc_shared *const shared =
	(volatile struct mpipe_ipc_shared *)DT_REG_ADDR(SHARED_NODE);

static struct mpipe_ipc_transport transport;
static struct ipc_ept endpoint;

static K_SEM_DEFINE(endpoint_bound, 0, 1);

/* What the DSP reports upstream after each decision. */
#define RESULT_PAYLOAD_BYTES 8U
#define RESULT_OFF_WINDOW    0U
#define RESULT_OFF_CATEGORY  4U

static atomic_t reported_window = ATOMIC_INIT(-1);
static atomic_t reported_category;

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
static int send_result(uint32_t window, uint32_t category)
{
	uint8_t frame[MPIPE_IPC_HEADER_LENGTH + RESULT_PAYLOAD_BYTES];
	uint8_t payload[RESULT_PAYLOAD_BYTES];
	struct mpipe_ipc_message message = {
		.header = { .cmd = MPIPE_IPC_CMD(MPIPE_IPC_TYPE_STATUS, 0U) },
		.payload = payload,
		.payload_length = sizeof(payload),
	};
	size_t written;
	int err;

	if (mpipe_ipc_transport_check_peer(&transport) != 0) {
		return -ECONNRESET;
	}

	sys_put_le32(window, &payload[RESULT_OFF_WINDOW]);
	sys_put_le32(category, &payload[RESULT_OFF_CATEGORY]);

	err = mpipe_ipc_encode(frame, sizeof(frame), &message, &written);
	if (err != 0) {
		return err;
	}

	/* ipc_service_send() returns the byte count, not zero. */
	err = ipc_service_send(&endpoint, frame, written);
	if (err < 0) {
		return err;
	}

	return ((size_t)err == written) ? 0 : -EIO;
}
#endif

static void on_bound(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&endpoint_bound);
}

static void on_received(const void *data, size_t length, void *priv)
{
	struct mpipe_ipc_message message;
	int err;

	ARG_UNUSED(priv);

	/* Never act on bytes from an incarnation we did not handshake with. */
	if (mpipe_ipc_transport_check_peer(&transport) != 0) {
		return;
	}

	err = mpipe_ipc_decode(&message, data, length);
	if (err != 0) {
		LOG_WRN("dropped a malformed message of %zu bytes: %d", length, err);
		return;
	}

	if (MPIPE_IPC_CMD_TYPE(message.header.cmd) == MPIPE_IPC_TYPE_STATUS &&
	    message.payload_length >= RESULT_PAYLOAD_BYTES) {
		atomic_set(&reported_category,
			   (atomic_val_t)sys_get_le32(
				   &message.payload[RESULT_OFF_CATEGORY]));
		atomic_set(&reported_window,
			   (atomic_val_t)sys_get_le32(
				   &message.payload[RESULT_OFF_WINDOW]));
	}
}

static const struct ipc_ept_cfg endpoint_cfg = {
	.name = "mpipe.ctrl",
	.cb = { .bound = on_bound, .received = on_received },
};

#ifdef CONFIG_SOC_MIMX8ML8_ADSP

/* Handed to the inference sink, so a result travels back up the control link. */
static void on_inference_result(uint32_t window, uint32_t category)
{
	if (send_result(window, category) != 0) {
		LOG_WRN("window %u: could not report upstream", window);
	}
}

#endif /* CONFIG_SOC_MIMX8ML8_ADSP */

int main(void)
{
	const struct device *ipc = DEVICE_DT_GET(IPC_NODE);
	bool control_registered = false;
	bool streaming = false;
	bool role_start_attempted = false;
	bool bound = false;
	int run_error = 0;
	int teardown_error = 0;
	int err;

	signal_linux_ready();

	LOG_INF("mpipe IPC inference: role=%s", IS_HOST ? "host" : "remote");

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
	model_runner_init();
	LOG_INF("micro_speech ready");
#endif

	if (!device_is_ready(ipc)) {
		LOG_ERR("IPC device not ready");
		return -ENODEV;
	}

	err = mpipe_ipc_transport_init(&transport, shared, &mpipe_ipc_zephyr_ops,
				       (void *)ipc, IS_HOST);
	if (err != 0) {
		LOG_ERR("transport init failed: %d", err);
		return err;
	}

	/* MU3 is clocked only while the DSP is up; see the bring-up sample. */
	(void)mpipe_ipc_transport_require_peer(&transport, true);

	do {
		err = mpipe_ipc_transport_poll(&transport);
		if (err == -EAGAIN) {
			k_msleep(20);
		}
	} while (err == -EAGAIN);

	if (err != 0) {
		LOG_ERR("bring-up failed: %d", err);
		return err;
	}

	LOG_INF("link up, session %u <-> %u", transport.session.local_sid,
		transport.session.remote_sid);

	k_sem_reset(&endpoint_bound);
	err = ipc_service_register_endpoint(ipc, &endpoint, &endpoint_cfg);
	if (err != 0) {
		LOG_ERR("cannot register control endpoint: %d", err);
		run_error = err;
		goto teardown;
	}
	control_registered = true;

	do {
		k_msleep(10);
		err = mpipe_ipc_transport_poll(&transport);

		if (err == 0 && !bound && k_sem_take(&endpoint_bound, K_NO_WAIT) == 0) {
			bound = true;
			LOG_INF("control endpoint bound");
		}

		if (err == 0 && transport.session.connected && !streaming) {
			role_start_attempted = true;
#ifdef CONFIG_SOC_MIMX8ML8_ADSP
			int start_err = consumer_start(ipc, &transport, on_inference_result);
#else
			int start_err = producer_start(ipc, &transport);
#endif

			if (start_err == 0) {
				streaming = true;
			} else {
				LOG_ERR("cannot start pipeline: %d", start_err);
				run_error = start_err;
				break;
			}
		}

		if (err == 0 && streaming) {
#ifdef CONFIG_SOC_MIMX8ML8_ADSP
			/* The pipeline runs itself; publish progress. */
			consumer_publish();
#else
			producer_publish();

			if (atomic_get(&reported_window) >= 0) {
				uint32_t w = (uint32_t)atomic_get(&reported_window);
				uint32_t c = (uint32_t)atomic_get(&reported_category);

				atomic_set(&reported_window, -1);
				LOG_INF("[%u] heard \"%s\"   (%u buffers dropped)", w,
					category_label(c), producer_dropped());
			}
#endif
		}
	} while (err == 0);

	if (err == -ECONNRESET) {
		LOG_WRN("peer restarted; stopping both halves of the link");
	} else if (err != 0) {
		run_error = err;
		LOG_ERR("transport failed: %d", err);
	}

teardown:
	if (role_start_attempted) {
#ifdef CONFIG_SOC_MIMX8ML8_ADSP
		err = consumer_stop();
#else
		err = producer_stop();
#endif
		if (err != 0 && teardown_error == 0) {
			teardown_error = err;
		}
	}
	if (control_registered) {
		err = ipc_service_deregister_endpoint(&endpoint);
		if (err != 0 && teardown_error == 0) {
			teardown_error = err;
		}
	}
	err = mpipe_ipc_transport_quiesce_with_error(&transport, teardown_error);
	if (err != 0 && teardown_error == 0) {
		teardown_error = err;
	}

	if (teardown_error == 0) {
		if (run_error == 0) {
			LOG_INF("link is DOWN; waiting for Linux to restart the M7/HiFi4 pair");
		} else {
			LOG_ERR("link stopped with error %d and is DOWN; waiting for paired reset",
				run_error);
		}
	} else {
		LOG_ERR("link teardown failed (%d); FAULT requires paired reset",
			teardown_error);
	}

	/* DOWN and FAULT are terminal here. Only the system owner may reset rings. */
	k_sleep(K_FOREVER);

	return teardown_error != 0 ? teardown_error : run_error;
}
