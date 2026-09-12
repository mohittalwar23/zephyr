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
#include <zephyr/mpipe/ipc/mpipe_ipc_ring.h>
#include <zephyr/mpipe/ipc/mpipe_ipc_transport.h>

LOG_MODULE_REGISTER(mpipe_ipc_infer, LOG_LEVEL_INF);

#include <fsl_mu.h>

#ifdef CONFIG_SOC_MIMX8ML8_ADSP
/* The C surface of the C++ inference code. */
void model_runner_init(void);
int micro_speech_process_audio(const int16_t *audio_data, size_t audio_data_size);
const char *micro_speech_category_label(int category);

#define LIVE_MIC 0
#define LIVE_I2S 0
#define LIVE_AUDIO 0

static void signal_linux_ready(void)
{
	MU_Type *const mu2_b = (MU_Type *)(uintptr_t)0x30e70000U;

	(void)MU_TriggerInterrupts(mu2_b, kMU_GenInt0InterruptTrigger);
}
#else
static inline void signal_linux_ready(void) { }

/*
 * Capture live when the board gives us a microphone, otherwise send the baked
 * clips. The clip path is what makes the sample self-checking -- it knows the
 * right answer -- so it stays the default; the microphone is what makes it a
 * demo.
 */
#define LIVE_MIC DT_NODE_HAS_STATUS(DT_NODELABEL(micfil), okay)
#define LIVE_I2S IS_ENABLED(CONFIG_SAMPLE_IPC_INFER_LIVE_I2S)
#define LIVE_AUDIO (LIVE_MIC || LIVE_I2S)

#if LIVE_I2S
#include "producer.h"
#elif LIVE_MIC
#include <zephyr/audio/dmic.h>
#else
#include "test_clips.h"
#endif
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
#define RING_NODE   DT_NODELABEL(mpipe_ipc_ring)

#define IS_HOST (DT_ENUM_IDX_OR(IPC_NODE, role, 0) == 0)

static volatile struct mpipe_ipc_shared *const shared =
	(volatile struct mpipe_ipc_shared *)DT_REG_ADDR(SHARED_NODE);
static volatile struct mpipe_ipc_ring_shared *const ring_shared =
	(volatile struct mpipe_ipc_ring_shared *)DT_REG_ADDR(RING_NODE);

/*
 * micro_speech wants 16 kHz mono, and it decides on one second at a time. A
 * 10 ms period keeps the ring's pacing the same as the audio format's, so one
 * decision is exactly 100 periods.
 */
#define SAMPLE_RATE_HZ     16000U
#define PERIOD_SAMPLES     160U
#define RING_PERIOD_BYTES  (PERIOD_SAMPLES * sizeof(int16_t))
#define RING_PERIOD_COUNT  64U
#define PERIODS_PER_WINDOW (SAMPLE_RATE_HZ / PERIOD_SAMPLES)
#define WINDOW_SAMPLES     (PERIODS_PER_WINDOW * PERIOD_SAMPLES)

BUILD_ASSERT(DT_REG_SIZE(RING_NODE) >=
		     MPIPE_IPC_RING_DATA_OFFSET +
			     (RING_PERIOD_BYTES * RING_PERIOD_COUNT),
	     "the reserved ring region is too small for this geometry");

static struct mpipe_ipc_transport transport;
static struct mpipe_ipc_ring ring;
static struct ipc_ept endpoint;

static K_SEM_DEFINE(endpoint_bound, 0, 1);

/* What the DSP reports upstream after each decision. */
#define RESULT_PAYLOAD_BYTES 8U
#define RESULT_OFF_WINDOW    0U
#define RESULT_OFF_CATEGORY  4U

static atomic_t reported_window = ATOMIC_INIT(-1);
static atomic_t reported_category;

static uint32_t ring_load(const volatile uint32_t *address)
{
	return *address;
}

static void ring_store(volatile uint32_t *address, uint32_t value)
{
	*address = value;
}

static const struct mpipe_ipc_ring_ops ring_ops = {
	.load = ring_load,
	.store = ring_store,
};

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
/* One second of audio, rebuilt from the ring a period at a time. */
static int16_t window[WINDOW_SAMPLES];
static uint32_t window_fill;
static uint32_t windows_done;

static void consume(void)
{
	while (true) {
		const int16_t *period = mpipe_ipc_ring_claim_read(&ring);
		int category;
		uint32_t started;
		uint32_t ms;

		if (period == NULL) {
			return;
		}

		memcpy(&window[window_fill], period, RING_PERIOD_BYTES);
		(void)mpipe_ipc_ring_commit_read(&ring);
		window_fill += PERIOD_SAMPLES;

		if (window_fill < WINDOW_SAMPLES) {
			continue;
		}

		window_fill = 0U;

		started = k_cycle_get_32();
		category = micro_speech_process_audio(window, WINDOW_SAMPLES);
		ms = k_cyc_to_ms_near32(k_cycle_get_32() - started);

		if (category < 0) {
			LOG_ERR("window %u: inference failed: %d", windows_done,
				category);
			continue;
		}

		LOG_INF("window %u: heard '%s' in %u ms", windows_done,
			micro_speech_category_label(category), ms);

		if (send_result(windows_done, (uint32_t)category) != 0) {
			LOG_WRN("window %u: could not report upstream",
				windows_done);
		}
		windows_done++;
	}
}
#else
#if LIVE_MIC && !LIVE_I2S

/*
 * MICFIL delivers interleaved stereo; micro_speech wants one channel. A block
 * is one ring period's worth of frames, so each capture fills exactly one
 * period and the two pacings stay locked together.
 */
#define MIC_CHANNELS    2U
/*
 * MICFIL's FIFO is 32 bits wide and only the top 24 bits carry signal, so the
 * driver hands back 32-bit samples. micro_speech wants 16-bit, which is the top
 * half of each word.
 */
#define MIC_SAMPLE_BYTES 4U
#define MIC_BLOCK_BYTES (PERIOD_SAMPLES * MIC_CHANNELS * MIC_SAMPLE_BYTES)
#define MIC_BLOCK_COUNT 8U

K_MEM_SLAB_DEFINE_STATIC(mic_slab, MIC_BLOCK_BYTES, MIC_BLOCK_COUNT, 32);

static const struct device *const mic = DEVICE_DT_GET(DT_NODELABEL(micfil));

static int mic_start(void)
{
	struct pcm_stream_cfg stream = {
		.pcm_width = 32,
		.pcm_rate = SAMPLE_RATE_HZ,
		.block_size = MIC_BLOCK_BYTES,
		.mem_slab = &mic_slab,
	};
	struct dmic_cfg cfg = {
		/* Bounds on what the microphone itself can be clocked at. */
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = MIC_CHANNELS,
			.req_chan_map_lo =
				dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
				dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT),
		},
	};
	int err;

	if (!device_is_ready(mic)) {
		LOG_ERR("microphone not ready");
		return -ENODEV;
	}

	err = dmic_configure(mic, &cfg);
	if (err != 0) {
		LOG_ERR("cannot configure the microphone: %d", err);
		return err;
	}

	err = dmic_trigger(mic, DMIC_TRIGGER_START);
	if (err != 0) {
		LOG_ERR("cannot start capture: %d", err);
		return err;
	}

	LOG_INF("capturing: %u Hz, %u channels, %u ms per period", SAMPLE_RATE_HZ,
		MIC_CHANNELS, (PERIOD_SAMPLES * 1000U) / SAMPLE_RATE_HZ);

	return 0;
}

/* Sum of squares over a period, so the caller can show a level. */
static uint64_t period_energy(const int16_t *samples)
{
	uint64_t energy = 0;

	for (uint32_t i = 0; i < PERIOD_SAMPLES; i++) {
		energy += (uint64_t)((int32_t)samples[i] * (int32_t)samples[i]);
	}

	return energy;
}

static uint64_t window_energy;
static uint32_t window_periods;

/*
 * Capture runs in its own thread with a bounded read.
 *
 * Not because threads are tidier: a zero timeout makes the driver's read find
 * its queue momentarily empty and take a fallback that returns *silence and
 * success*, so a polling caller never blocks, never fails, and fills the ring
 * with zeros as fast as it can loop. The read has to be allowed to wait, and
 * waiting on the link's own loop would stall peer detection.
 */
#define CAPTURE_READ_TIMEOUT_MS 100
#define CAPTURE_STACK_SIZE      2048

static K_THREAD_STACK_DEFINE(capture_stack, CAPTURE_STACK_SIZE);
static struct k_thread capture_thread;
static atomic_t capture_live;

static void capture_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (true) {
		void *block;
		uint32_t size;
		const int32_t *frames;
		int16_t *slot;
		int err;

		err = dmic_read(mic, 0, &block, &size, CAPTURE_READ_TIMEOUT_MS);
		if (err != 0) {
			continue;
		}

		/* The ring is torn down and rebuilt around a peer restart. */
		if (atomic_get(&capture_live) == 0) {
			k_mem_slab_free(&mic_slab, block);
			continue;
		}

		slot = mpipe_ipc_ring_claim_write(&ring);
		if (slot == NULL) {
			/* The DSP is behind. Drop this period and keep capturing. */
			mpipe_ipc_ring_record_overrun(&ring);
			k_mem_slab_free(&mic_slab, block);
			continue;
		}

		/*
		 * Keep the left channel and take the top 16 bits of each
		 * 32-bit word -- micro_speech is 16-bit mono.
		 */
		frames = block;
		for (uint32_t i = 0; i < PERIOD_SAMPLES; i++) {
			slot[i] = (int16_t)(frames[i * MIC_CHANNELS] >> 16);
		}

		window_energy += period_energy(slot);
		window_periods++;

		(void)mpipe_ipc_ring_commit_write(&ring);
		k_mem_slab_free(&mic_slab, block);
	}
}

static void capture_thread_start(void)
{
	(void)k_thread_create(&capture_thread, capture_stack, CAPTURE_STACK_SIZE,
			      capture_entry, NULL, NULL, NULL,
			      K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&capture_thread, "mic capture");
}

/* The link loop owns nothing on the capture side; it only gates the ring. */
static void produce(void)
{
}

/* Root-mean-square of the last window, as a rough level indicator. */
static uint32_t take_window_level(void)
{
	uint64_t mean;
	uint32_t root = 0;

	if (window_periods == 0U) {
		return 0U;
	}

	mean = window_energy / ((uint64_t)window_periods * PERIOD_SAMPLES);
	window_energy = 0U;
	window_periods = 0U;

	/* Integer square root; the value is only ever read by a human. */
	while ((uint64_t)(root + 1U) * (root + 1U) <= mean) {
		root++;
	}

	return root;
}

#elif !LIVE_I2S

/*
 * Send a different word each window, and remember which. Streaming one clip on
 * repeat would prove only that the remote keeps answering -- not that it is
 * listening: a stuck classifier scores the same as a working one. Cycling the
 * three clips makes a wrong answer visible.
 */
static const struct {
	const int16_t *samples;
	const char *name;
	uint32_t category;
} clips[] = {
	{ yes_clip, "yes", 2U },
	{ no_clip,  "no",  3U },
};

/*
 * Two words, not three: the M7's rodata lives in ITCM, which cannot hold a
 * third second of 16-bit PCM. Distinguishing "yes" from "no" is the part that
 * matters anyway -- both are speech, so a classifier that has stopped listening
 * cannot score on it by accident.
 */

static uint32_t periods_sent;

/* Which clip window N carries, so the answer can be checked against it. */
static uint32_t clip_for_window(uint32_t window)
{
	return window % ARRAY_SIZE(clips);
}

static void produce(void)
{
	for (uint32_t i = 0; i < 8U; i++) {
		int16_t *slot = mpipe_ipc_ring_claim_write(&ring);
		uint32_t window;
		uint32_t offset;
		const int16_t *clip;

		if (slot == NULL) {
			mpipe_ipc_ring_record_overrun(&ring);
			return;
		}

		/* Each window is exactly one clip, start to end. */
		window = periods_sent / PERIODS_PER_WINDOW;
		offset = (periods_sent % PERIODS_PER_WINDOW) * PERIOD_SAMPLES;
		clip = clips[clip_for_window(window)].samples;

		memcpy(slot, &clip[offset], RING_PERIOD_BYTES);
		periods_sent++;

		(void)mpipe_ipc_ring_commit_write(&ring);
	}
}

#endif /* LIVE_MIC */
#endif /* CONFIG_SOC_MIMX8ML8_ADSP */

int main(void)
{
	const struct device *ipc = DEVICE_DT_GET(IPC_NODE);
	unsigned int generation = 0;
	bool streaming;
	bool bound;
#if !defined(CONFIG_SOC_MIMX8ML8_ADSP) && LIVE_AUDIO
	bool capturing = false;
#endif
#if !defined(CONFIG_SOC_MIMX8ML8_ADSP) && !LIVE_AUDIO
	uint32_t correct = 0;
	uint32_t wrong = 0;
#endif
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

	while (true) {
		do {
			err = mpipe_ipc_transport_poll(&transport);
			if (err == -EAGAIN) {
				k_msleep(20);
			}
		} while (err == -EAGAIN);

		if (err != 0) {
			LOG_ERR("gen %u: bring-up failed: %d", generation, err);
			return err;
		}

		LOG_INF("gen %u: link up, session %u <-> %u", generation,
			transport.session.local_sid, transport.session.remote_sid);

		k_sem_reset(&endpoint_bound);
		err = ipc_service_register_endpoint(ipc, &endpoint, &endpoint_cfg);
		if (err != 0) {
			LOG_ERR("gen %u: cannot register endpoint: %d", generation,
				err);
			return err;
		}

		streaming = false;
		bound = false;

		do {
			k_msleep(10);
			err = mpipe_ipc_transport_poll(&transport);

			if (err == 0 && !bound &&
			    k_sem_take(&endpoint_bound, K_NO_WAIT) == 0) {
				bound = true;
				LOG_INF("gen %u: endpoint bound", generation);
			}

			if (err == 0 && transport.session.connected && !streaming) {
				int ring_err = mpipe_ipc_ring_init(
					&ring, ring_shared, &ring_ops, IS_HOST,
					RING_PERIOD_BYTES, RING_PERIOD_COUNT,
					DT_REG_SIZE(RING_NODE));

				if (ring_err == 0) {
					streaming = true;
					LOG_INF("gen %u: ring up, %u x %u bytes",
						generation, RING_PERIOD_COUNT,
						(unsigned int)RING_PERIOD_BYTES);
#if LIVE_I2S
					if (!capturing) {
						if (producer_start(ipc) != 0) {
							return -EIO;
						}
						capturing = true;
					}
#elif LIVE_MIC
					if (!capturing) {
						if (mic_start() != 0) {
							return -EIO;
						}
						capture_thread_start();
						capturing = true;
					}
					atomic_set(&capture_live, 1);
#endif
				} else if (ring_err != -EAGAIN) {
					LOG_ERR("gen %u: ring refused: %d",
						generation, ring_err);
				}
			}

			if (err == 0 && streaming) {
#ifdef CONFIG_SOC_MIMX8ML8_ADSP
				consume();
#else
#if !LIVE_I2S
				produce();
#endif

				if (atomic_get(&reported_window) >= 0) {
					uint32_t w =
						(uint32_t)atomic_get(&reported_window);
					uint32_t c =
						(uint32_t)atomic_get(&reported_category);

					atomic_set(&reported_window, -1);
#if LIVE_I2S
					/*
					 * No expected answer with live audio.
					 * The audible branch is the microphone
					 * check; this is the model's verdict.
					 */
					LOG_INF("[%u] heard \"%s\"   (%u periods dropped)",
						w, category_label(c), producer_dropped());
#elif LIVE_MIC
					/*
					 * Nothing to check against a live
					 * microphone -- just say what came back,
					 * with a level so a silent microphone is
					 * distinguishable from a quiet room.
					 */
					LOG_INF("[%u] heard \"%s\"   (level %u)", w,
						category_label(c), take_window_level());
#else
					{
						uint32_t sent = clip_for_window(w);

						if (c == clips[sent].category) {
							correct++;
						} else {
							wrong++;
							LOG_ERR("window %u: sent '%s', "
								"remote heard category %u",
								w, clips[sent].name, c);
						}

						if ((correct + wrong) % 15U == 0U) {
							LOG_INF("gen %u: %u windows "
								"correct, %u wrong",
								generation, correct,
								wrong);
						}
					}
#endif
				}
#endif
			}
		} while (err == 0);

#if LIVE_MIC && !LIVE_I2S
		atomic_set(&capture_live, 0);
#endif
		LOG_WRN("gen %u: peer restarted; standing down", generation);
		(void)ipc_service_deregister_endpoint(&endpoint);
		(void)mpipe_ipc_transport_quiesce(&transport);
		k_msleep(200);
		generation++;

		err = mpipe_ipc_transport_rebuild(&transport);
		if (err != 0) {
			LOG_ERR("gen %u: cannot rebuild: %d", generation, err);
			return err;
		}
	}

	return 0;
}
