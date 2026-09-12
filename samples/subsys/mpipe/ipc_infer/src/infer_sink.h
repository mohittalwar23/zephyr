/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MPIPE_IPC_INFER_SINK_H_
#define MPIPE_IPC_INFER_SINK_H_

#include <stdint.h>

#include <zephyr/mpipe/mpipe_sink.h>

/* micro_speech decides on one second of 16 kHz audio at a time. */
#define INFER_WINDOW_SAMPLES 16000

/** Called with the category each time a window has been classified. */
typedef void (*infer_result_cb)(uint32_t window, uint32_t category);

/** Sink that collects a window of audio and classifies it. */
struct infer_sink {
	struct mpipe_sink base;
	int16_t window[INFER_WINDOW_SAMPLES];
	uint32_t fill;
	uint32_t windows;
	uint32_t channels;
	infer_result_cb on_result;
};

int infer_sink_init(struct infer_sink *infer, uint8_t id, uint32_t channels,
		    infer_result_cb on_result);

#endif /* MPIPE_IPC_INFER_SINK_H_ */
