.. zephyr:code-sample:: audio-pipeline
   :name: Audio pipeline (libMP)

   Build an audio pipeline that captures PCM audio, applies gain control and
   plays it back, using the libMP (Media Pipe) subsystem.

Overview
********

This sample builds a small audio pipeline on top of the libMP (Media Pipe)
subsystem. Audio is captured from a digital microphone (DMIC),
passed through a caps filter and a gain transform, then played back through an
I2S codec sink:

.. mermaid::

   graph LR
       A[DMIC source] --> B[Caps filter]
       B --> C[Gain transform]
       C --> D[I2S codec sink]

Each element is a libMP plugin (``zaud``). The application only constructs the
elements, sets their properties, links them and starts the pipeline; buffer
allocation, caps negotiation and data flow are handled by libMP.

native_sim file-in / file-out loopback
***************************************

On :zephyr:board:`native_sim <native_sim>` the audio drivers are file-backed, so the
whole pipeline runs without any hardware as a deterministic loopback:

.. code-block:: none

   dmic_in.pcm  ->  DMIC  ->  caps filter  ->  gain  ->  I2S TX  ->  i2s_out.pcm

* The DMIC reads raw little-endian PCM from ``dmic_in.pcm`` (16 kHz, 16-bit,
  stereo). If the file is absent the DMIC produces silence, so the pipeline
  still runs.
* The I2S sink writes the processed PCM to ``i2s_out.pcm``.
* The pipeline captures ``CONFIG_AUDIO_PIPELINE_NUM_FRAMES``
  frames, reports end-of-stream and stops cleanly.

This makes the sample usable as CI / regression validation infrastructure for
libMP audio without a board.

.. note::

   Audio drivers (DMIC, I2S) manage their buffers through a private
   :c:struct:`k_mem_slab` and there is no common audio buffer-management
   framework yet, so the ``zaud`` elements copy between the driver slab and the
   libMP transport buffer at each driver boundary. A zero-copy pure-audio path
   (shared pool / RTIO) is future work tracked by
   `zephyr#107868 <https://github.com/zephyrproject-rtos/zephyr/issues/107868>`_.

Building and running
********************

native_sim
==========

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/audio/pipeline
   :board: native_sim/native/64
   :goals: build
   :compact:

Optionally generate a test tone for the DMIC input:

.. code-block:: console

   python3 samples/drivers/audio/pipeline/scripts/gen_input.py \
       --frames 150 --out dmic_in.pcm

Run the simulator (it reads ``dmic_in.pcm`` and writes ``i2s_out.pcm`` in the
current directory):

.. code-block:: console

   ./build/zephyr/zephyr.exe

The input and output file paths can be overridden at run time:

.. code-block:: console

   ./build/zephyr/zephyr.exe --dmic0_file=/path/to/in.pcm \
       --i2s_rxtx_tx=/path/to/out.pcm

Expected output:

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   <inf> main: Starting audio pipeline (100 frames)
   <inf> mp_zaud_dmic_src: DMIC capture started
   <inf> main: EOS received from element 1
   <inf> main: Pipeline stopped

With the default 90% gain, every output sample is 0.9x the corresponding input
sample.

Verifying the output
====================

``scripts/verify_output.py`` re-implements the gain element's Q16.16 math
independently and checks the captured output (frame count, exact gain,
mute/unity, clipping, silence):

.. code-block:: console

   python3 samples/drivers/audio/pipeline/scripts/verify_output.py \
       --in dmic_in.pcm --out i2s_out.pcm --gain 90 --frames 100

Testing
*******

The pipeline ships a full Twister matrix that runs on ``native_sim`` with no
hardware. It covers the lifecycle (start / EOS / clean stop), a gain sweep
(0 / 50 / 90 / 100 / 200 / 1000 %), missing-input silence, short-stream flush,
and a sanitizer (ASAN + UBSAN) build. The pytest harness
(``pytest/test_pipeline.py``) generates input, runs the binary and asserts the
output against ``verify_output.py``:

.. code-block:: console

   ./scripts/twister -p native_sim/native/64 -T samples/drivers/audio/pipeline

Long-running soak / stress runs (not part of CI) are driven by
``scripts/run_matrix.sh``, which checks byte-exact output, exact frame count and zero simulated-clock drift over many frames:

.. code-block:: console

   samples/drivers/audio/pipeline/scripts/run_matrix.sh --frames 100000        # fast-forward
   samples/drivers/audio/pipeline/scripts/run_matrix.sh --frames 100000 --rt   # host real time
