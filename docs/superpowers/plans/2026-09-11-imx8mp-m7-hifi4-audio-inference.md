# i.MX8MP M7-to-HiFi4 Audio and Inference Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Do not delegate implementation unless the user explicitly authorizes it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect the proven direct transport to reusable mpipe sink/source elements, then deliver deterministic stereo-to-mono aggregation and attributed micro-speech inference without violating real-time ownership rules.

**Architecture:** The M7 mpipe graph terminates in an injected IPC sink that copies one validated 10 ms stereo frame into a nonblocking transport buffer. The HiFi4 IPC source holds transport receive buffers only until a worker converts them to owned 20 ms mono blocks; a separate model worker maintains the one-second inference window. Model import is isolated from the new transport and preserves original authorship and license notices.

**Tech Stack:** Zephyr mpipe/net_buf, ztest/native_sim, IPC transport from the preceding plan, DMIC, C/C++, TensorFlow Lite Micro, Xtensa HiFi4.

**Spec:** `docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`

## Entry gate and constraints

- The deterministic two-core transport plan must be green, including pressure, paired restart, and stale-session rejection on hardware.
- Reusable elements receive a transport/device/config pointer at initialization; they must not hide `DEVICE_DT_GET`, endpoint singletons, or global mutable session state.
- Capture, mailbox, and IPC receive callbacks do not block, allocate, infer, or wait for a transport buffer.
- Wire format remains 16 kHz, signed 16-bit little-endian, stereo interleaved, 160 samples/channel, 10 ms. Conversion happens only on HiFi4.
- Keep the tested audio-loopback branch intact. No codec output or speaker path is enabled.
- Reuse only reviewed content from original PR #96657 commit `101fb10a559e07cfad4d90c04687d0d1c9803e0b`; do not import its RPMsg transport or copy from `gsoc/mp-ipc-plugin`/`69cb3b08a12a`.

---

### Task 1: Implement independent mpipe IPC sink instances

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_sink.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_sink.c`
- Modify: `subsys/mpipe/ipc/Kconfig`
- Modify: `subsys/mpipe/ipc/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_elements/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_elements/prj.conf`
- Create: `tests/subsys/mpipe/ipc_elements/tests.yaml`
- Create: `tests/subsys/mpipe/ipc_elements/src/test_sink.c`
- Create: `tests/subsys/mpipe/ipc_elements/src/fake_transport.c`

**Interfaces:**

```c
struct mpipe_ipc_sink_config {
	struct mpipe_ipc_transport *transport;
	uint32_t stream_id;
	struct mpipe_ipc_audio_format format;
};

int mpipe_ipc_sink_init(struct mpipe_ipc_sink *sink, uint8_t id,
			const struct mpipe_ipc_sink_config *config);
void mpipe_ipc_sink_get_stats(const struct mpipe_ipc_sink *sink,
			      struct mpipe_ipc_sink_stats *stats);
```

- [ ] **Step 1: Add failing element and ownership tests**

Instantiate two sinks against two fake transports and prove their counters,
credits, and lifecycle do not cross. Cover caps rejection, state transitions,
one frame/one submission, credit check before TX acquisition, `K_NO_WAIT`, one
copy, newest-frame drop on no credit/no buffer, mpipe input unref exactly once,
and transport-fatal propagation after an acquired-buffer send failure.

```c
ZTEST(mpipe_ipc_elements, test_two_sinks_are_independent)
{
	struct mpipe_ipc_sink left;
	struct mpipe_ipc_sink right;
	/* Initialize with distinct fake transports and push one frame to left. */
	zassert_equal(fake_left.sent, 1);
	zassert_equal(fake_right.sent, 0);
}
```

- [ ] **Step 2: Run the suite and verify the missing-element failure**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_elements -p native_sim/native/64
```

- [ ] **Step 3: Implement the minimal sink on the existing mpipe base**

Initialize `struct mpipe_sink`, install caps/state/chain functions, and store the
injected config inside the instance. The chain function validates the exact
negotiated caps and delegates to the transport without retaining the mpipe
buffer. It returns promptly for both accepted and dropped frames and exposes
drop/fatal counters through a copy-out accessor.

- [ ] **Step 4: Run tests and commit the sink**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_elements -p native_sim/native/64
git diff --check
git add include/zephyr/mpipe/ipc subsys/mpipe/ipc tests/subsys/mpipe/ipc_elements
git commit -s -m "subsys: mpipe: add nonblocking IPC audio sink"
```

### Task 2: Implement the HiFi4 IPC source and stereo adapter

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_src.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_src.c`
- Create: `include/zephyr/mpipe/aud/mpipe_aud_stereo_to_mono.h`
- Create: `subsys/mpipe/aud/mpipe_aud_stereo_to_mono.c`
- Modify: `subsys/mpipe/ipc/CMakeLists.txt`
- Modify: `subsys/mpipe/aud/CMakeLists.txt`
- Modify: `subsys/mpipe/aud/Kconfig`
- Create: `tests/subsys/mpipe/ipc_elements/src/test_src.c`
- Create: `tests/subsys/mpipe/ipc_elements/src/test_stereo_to_mono.c`

**Interfaces:**

```c
int mpipe_ipc_src_init(struct mpipe_ipc_src *src, uint8_t id,
		       const struct mpipe_ipc_src_config *config);
int mpipe_aud_stereo_to_mono_init(struct mpipe_aud_stereo_to_mono *convert,
				  uint8_t id,
				  struct mpipe_buffer_pool *output_pool);
void mpipe_aud_stereo_to_mono_discontinuity(
		struct mpipe_aud_stereo_to_mono *convert);
```

- [ ] **Step 1: Write failing RX ownership and arithmetic tests**

Cover RX callback hold/queue/return, queue-full immediate release, worker copy
into owned mpipe storage, transport release exactly once, credit only after
release, stop drain, and two independent sources. Arithmetic vectors include
`INT16_MIN`, `INT16_MAX`, opposite signs, odd sums, and all 320 stereo pairs.
Expected mono uses a signed 32-bit intermediate and C division `(left + right) /
2`; no 16-bit overflow or unsigned rounding is allowed.

- [ ] **Step 2: Add failing aggregation and reset tests**

Two valid 10 ms frames produce one 20 ms/320-sample mono buffer. One frame
alone produces no output. Session change, sequence gap, DISCONTINUITY flag,
format error, STOP, and FAULT each discard the partial first half. Explicitly
prove halves from different sessions can never share an output buffer.

- [ ] **Step 3: Implement bounded callback plus worker conversion**

The IPC callback performs validated hold plus fixed-depth `k_msgq_put` with
`K_NO_WAIT`. The worker converts/copies, pushes the owned mpipe buffer, then
releases the transport buffer and returns credit. Make output pool exhaustion a
counted newest-frame drop, not a callback wait. Reset partial aggregation before
processing the next valid frame after any discontinuity.

- [ ] **Step 4: Run all element tests and commit**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_elements -p native_sim/native/64
git diff --check
git add include/zephyr/mpipe subsys/mpipe tests/subsys/mpipe/ipc_elements
git commit -s -m "subsys: mpipe: add IPC source and mono aggregation"
```

### Task 3: Prove the elements with synthetic transport audio

**Files:**
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/CMakeLists.txt`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/Kconfig`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/prj.conf`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/main.c`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/pattern_source.c`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/verify_sink.c`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_m7.conf`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_m7.overlay`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_adsp.conf`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_adsp.overlay`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/README.rst`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/tests.yaml`

- [ ] **Step 1: Build a deterministic graph without DMIC or model**

M7 graph is pattern source to IPC sink. DSP graph is IPC source to stereo-mono
adapter to verifying sink. Each 10 ms frame encodes sequence-dependent left and
right samples; DSP checks every resulting 20 ms sample and prints bounded
counters only. Configure fixed pools/queues at compile time.

- [ ] **Step 2: Build both roles fresh and review memory budgets**

```bash
audio_m7_build=$(mktemp -d /tmp/imx8mp-audio-m7.XXXXXX)
audio_dsp_build=$(mktemp -d /tmp/imx8mp-audio-dsp.XXXXXX)
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc build -p always \
  -d "$audio_m7_build" -b imx8mp_evk/mimx8ml8/m7 \
  samples/subsys/mpipe/imx8mp_m7_hifi4_audio
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc build -p always \
  -d "$audio_dsp_build" -b imx8mp_evk/mimx8ml8/adsp \
  samples/subsys/mpipe/imx8mp_m7_hifi4_audio
```

Record ROM/RAM, stack analysis, pool/queue counts, ELF segments, shared map,
and hashes. Reject overlap or unexplained growth before a board packet.

- [ ] **Step 3: Obtain separate approval and run synthetic hardware tests**

Run clean, saturation, discontinuity, stop/drain, paired restart, and 30-minute
synthetic soak cases. Require exact output samples, zero ownership leaks, and
balanced credits. Record maximum callback/worker duration and queue high-water
marks. Stop on an unexplained late frame or cross-session aggregate.

- [ ] **Step 4: Commit the synthetic sample and evidence reference**

```bash
git add samples/subsys/mpipe/imx8mp_m7_hifi4_audio
git commit -s -m "samples: mpipe: add direct synthetic audio pipeline"
```

### Task 4: Connect the existing M7 DMIC capture graph

**Files:**
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/main.c`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_m7_dmic.conf`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/boards/imx8mp_evk_mimx8ml8_m7_dmic.overlay`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/tests.yaml`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/README.rst`

- [ ] **Step 1: Reuse the tested capture topology, not its output path**

Take DMIC pinctrl/clock/caps configuration from the current tested
`samples/subsys/mpipe/audio_loopback` files in this base. Build
`mpipe_aud_dmic_src -> mpipe_ipc_sink` at 16 kHz, stereo, S16_LE, 160 samples
per channel. Do not instantiate gain, I2S codec sink, SAI output, WM8960 output,
or any speaker route.

- [ ] **Step 2: Add build assertions and captured-frame metrics**

Assert caps equal the negotiated descriptor. Record capture callback duration,
frames captured/sent, no-credit/no-buffer drops, transport faults, and maximum
queue depth. Rate-limit logs outside callbacks.

- [ ] **Step 3: Build, present a DMIC-only board packet, and wait**

The packet must show the generated DTS proving MICFIL ownership and absence of
speaker/codec output. After approval, run silence/tone/speech capture with the
DSP verifier but no model, plus deliberate receiver pressure. Require bounded
newest-frame drops and recovery only through a new paired session.

- [ ] **Step 4: Commit the DMIC graph**

```bash
git add samples/subsys/mpipe/imx8mp_m7_hifi4_audio
git commit -s -m "samples: mpipe: route i.MX8MP DMIC over direct IPC"
```

### Task 5: Import the micro-speech model with preserved authorship

**Files:**
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/audio_preprocessor_int8_model.cpp`
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/audio_preprocessor_int8_model.hpp`
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/micro_model_settings.h`
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/micro_speech_quantized_model.cpp`
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/micro_speech_quantized_model.hpp`
- Create from original commit and adapt separately: `samples/modules/tflite-micro/micro_speech/src/inference/model_runner.cpp`
- Create from original commit: `samples/modules/tflite-micro/micro_speech/src/inference/model_runner.hpp`
- Create independently: `samples/modules/tflite-micro/micro_speech/CMakeLists.txt`
- Create independently: `samples/modules/tflite-micro/micro_speech/Kconfig`
- Create independently: `samples/modules/tflite-micro/micro_speech/prj.conf`
- Create independently: `samples/modules/tflite-micro/micro_speech/README.rst`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/CMakeLists.txt`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/prj.conf`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/tests.yaml`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/src/fixtures.c`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/src/fixtures.h`
- Create: `samples/modules/tflite-micro/micro_speech/tests/reference/src/main.cpp`

- [ ] **Step 1: Review and record the precise provenance diff**

```bash
git show --stat --format=fuller 101fb10a559e07cfad4d90c04687d0d1c9803e0b
git ls-tree -r --name-only 101fb10a559e07cfad4d90c04687d0d1c9803e0b \
  samples/modules/tflite-micro/micro_speech/src/inference
git diff --stat 101fb10a559e07cfad4d90c04687d0d1c9803e0b^ \
  101fb10a559e07cfad4d90c04687d0d1c9803e0b -- \
  samples/modules/tflite-micro/micro_speech/src/inference
```

Record that the original author is `Thong Phan <quang.thong2001@gmail.com>` and
that imported files retain the TensorFlow Authors copyright and Apache-2.0
text. Exclude `src/transport/rpmsg_transport.*`, old boards, and old main-loop
transport glue.

- [ ] **Step 2: Restore the reviewed inference files and preserve authorship**

```bash
git restore --source=101fb10a559e07cfad4d90c04687d0d1c9803e0b -- \
  samples/modules/tflite-micro/micro_speech/src/inference
git add samples/modules/tflite-micro/micro_speech/src/inference
git commit --author="Thong Phan <quang.thong2001@gmail.com>" -s \
  -m "samples: tflite-micro: import attributed micro-speech inference" \
  -m "Import only the inference subset from original commit 101fb10a559e07cfad4d90c04687d0d1c9803e0b." \
  -m "Signed-off-by: Thong Phan <quang.thong2001@gmail.com>"
```

The commit body must cite original commit
`101fb10a559e07cfad4d90c04687d0d1c9803e0b`, state that only the
inference subset was imported, and retain Thong's original Signed-off-by in
addition to the committer's DCO sign-off.

- [ ] **Step 3: Add failing fixed-fixture reference tests**

Use repository-owned, license-reviewed one-second PCM fixtures for silence,
unknown, yes, and no. Assert `model_runner_init()` succeeds once, arena bounds
are respected, `micro_speech_process_audio()` receives exactly 16,000 mono
samples, and outputs match recorded class/score tolerances. Keep fixtures small
and source-documented; do not fetch test data at build time.

- [ ] **Step 4: Adapt the runner behind a narrow application API**

Place project-authored adapter changes in a new commit after the preserved
import. Independently create the CMake/Kconfig/prj.conf/test harness and README;
do not restore the original transport-coupled build files. Inference runs on a
dedicated worker, never the IPC callback. Replace
transport-coupled output with a bounded result structure/callback containing
class, score, timestamp, session, and window sequence. Reset the model window on
session/discontinuity/stop and require one complete 16,000-sample window.

- [ ] **Step 5: Build/run fixtures and inspect attribution**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T samples/modules/tflite-micro/micro_speech/tests/reference \
  -p native_sim/native/64
git log --format='%H%x09%an%x09%ae%x09%s%n%b' -- \
  samples/modules/tflite-micro/micro_speech/src/inference
git diff --check
```

If native_sim cannot link the selected TFLM configuration, make this a target
build plus a host golden-vector helper, explicitly label it build-only, and do
not weaken the fixed expected outputs.

- [ ] **Step 6: Commit the project-authored adapter separately**

```bash
git add samples/modules/tflite-micro/micro_speech
git commit -s -m "samples: tflite-micro: decouple micro-speech input"
```

### Task 6: Integrate and verify live direct inference

**Files:**
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/CMakeLists.txt`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/Kconfig`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/main.c`
- Create: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/src/model_sink.cpp`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/README.rst`
- Modify: `samples/subsys/mpipe/imx8mp_m7_hifi4_audio/tests.yaml`

- [ ] **Step 1: Add transported-fixture mode before live speech**

M7 transmits the same reference fixtures through the real sink/transport. DSP
source/converter/model must produce the same class/score tolerances as the
local reference test. Inject gaps at every possible 10 ms boundary and prove no
mixed or partial one-second window is inferred.

- [ ] **Step 2: Build final firmware and present the production-firmware gate**

Record fresh test outputs, ROM/RAM/stacks, model arena, worker priorities,
callback/queue metrics, ELF segments, DTS/config, source/HAL/model provenance,
and SHA-256. Ask explicit approval before installing these production candidate
firmwares.

- [ ] **Step 3: Run ascending hardware evidence**

After approval: transported fixtures, silence/noise, repeated yes/no/unknown,
receiver pressure during capture, session restart between half-windows,
management-triggered stop, DSP fault, and 60-minute soak. Require stable scores
for fixtures, bounded live latency, no callback inference, zero leaks, correct
drop accounting, and fresh sessions after every recovery.

- [ ] **Step 4: Keep optimization optional and independently reviewable**

First record reference cycles, stack, arena, and output scores. Any HiFi4/NatureDSP
or kernel optimization goes in a separate commit and must pass bit-exact or
approved-tolerance fixture equivalence plus cycle improvement. If it does not,
drop that commit without changing the accepted reference path.

- [ ] **Step 5: Commit integration and record the audio gate**

```bash
git diff --check
git add samples/subsys/mpipe/imx8mp_m7_hifi4_audio
git commit -s -m "samples: mpipe: add direct HiFi4 speech inference"
```

The evidence record separates synthetic, DMIC, reference-model, transported-
fixture, and live-speech results. Any attribution ambiguity, cross-session
window, ownership leak, or unbounded callback blocks Linux production rollout.
