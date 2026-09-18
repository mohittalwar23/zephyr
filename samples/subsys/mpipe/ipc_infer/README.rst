.. zephyr:code-sample:: mpipe-ipc-infer
   :name: mpipe keyword spotting across a direct M7 to HiFi4 link

   Run one mpipe pipeline across two cores: capture on the Cortex-M7, hand the
   buffers to the HiFi4 through shared memory, and spot keywords there with
   micro_speech -- with no relay through Linux.

Overview
********

This is the end-to-end path the direct link exists for. One mpipe pipeline
spans both cores:

.. code-block:: none

   i2s_src -> caps_filter -> tee -+-> gain -> i2s_codec_sink      (M7, audible)
                                  |
                                  +-> ipc_sink ==== shared DDR ====\
                                                                    |
                                              ipc_src -> queue -> infer_sink
                                                        (HiFi4, micro_speech)

Both images are built from the same source; only the board files differ, and
only the DSP builds inference. The M7 captures 16 kHz audio from the codec,
hands each buffer to the HiFi4 **by reference** -- the samples stay in shared
memory and are never copied -- and the HiFi4 gathers them into one-second
windows, runs micro_speech, and reports the category back over the control
endpoint.

The audible branch is not decoration. Keyword spotting tells you only whether
the model agreed with you; hearing the same audio tells you whether the
microphone, the clocks and the gain are doing anything at all, which is a
different question and the one that fails first.

The split is the same one the bring-up sample establishes: control messages go
through IPC Service over MU3, and audio never touches that path.

Inference sources
*****************

The model and its runner are not copied here. They come from the micro_speech
sample at ``samples/modules/tflite-micro/micro_speech``, which is Zephyr pull
request #96657 and is not merged yet; until it is, it has to be applied to the
tree for this sample to build. The build fails with an explanation, and the
command to fetch it, if it is missing.

Three changes that sample needs are made in it rather than in a copy, and
belong upstream:

* ``micro_speech_process_audio()`` returned a status and logged the label
  internally, so a caller could not act on the classification. It now returns
  the category.
* ``model_runner.cpp`` included that sample's rpmsg transport, tying inference
  to the way audio happened to arrive.
* Its CMakeLists found the TFLM signal sources through a path relative to
  ``ZEPHYR_BASE``, which is wrong in a git worktree.

Because the audio is live, what this sample demonstrates is the path, not the
model's accuracy: say "yes" or "no" into the microphone and watch the category
come back across the link. A deterministic, machine-checkable stream through the
same plugin would be a better regression test, and is not here yet.

Requirements
************

The optional ``tflite-micro`` module, which is not fetched by default::

   west update tflite-micro

Everything the bring-up sample requires applies here too -- the reserved shared
window, the start order, and the MU3 clock's dependence on the DSP being
runtime-resumed. See :zephyr:code-sample:`mpipe-ipc-bringup`.

Restart policy
**************

This sample deliberately uses paired fail-stop recovery. If either core's
transport session changes, the survivor stops and joins its pipeline, cancels
plugin retry work, deregisters ``mpipe.audio`` and then ``mpipe.ctrl``, and
closes the IPC Service instance. It then remains in ``DOWN`` (or ``FAULT`` if
teardown failed) without rebuilding the static vrings.

Linux must restart both the Cortex-M7 and HiFi4. Restarting only one core is not
supported: a session change rejects stale messages, but cannot prove that the
failed peer stopped reading a zero-copy buffer. Outstanding producer references
therefore remain quarantined until the paired reset supplies that lifetime
boundary.

Memory
******

Same 256 KiB reservation at ``0xa0000000`` as the bring-up sample:

=============  ========  =====================================================
Address        Size      Contents
=============  ========  =====================================================
``0xa0000000``    4 KiB  Bring-up control block
``0xa0010000``   64 KiB  IPC Service shared memory
``0xa0020000``  128 KiB  Audio buffer pool, readable by both cores
=============  ========  =====================================================

The third region is what makes the handover zero-copy, and it is why the M7's
pool is placed by devicetree rather than linked wherever it lands: a buffer
handed over by reference has to live where the peer can read it. The M7 points
``zephyr,mpipe-aud-pool`` at that region, and both halves are told its bounds at
init, so an offset that falls outside it is rejected rather than followed.

Every region is a power-of-two size at a naturally aligned base. That is not
tidiness: the M7's ARMv7-M MPU rounds a region size up to the next power of two
and masks the base to match, so an odd region silently covers something other
than what was asked for.

Sample output
*************

On the M7::

   *** Booting Zephyr OS build v4.4.0 ***
   <inf> mpipe_ipc_infer: mpipe IPC inference: role=host
   <inf> mpipe_ipc_infer: link up, session 2 <-> 1
   <inf> mpipe_ipc_infer: control endpoint bound
   <inf> mpipe_ipc_plugin_sink: peer source bound
   <inf> mpipe_ipc_infer: capture pipeline: i2s -> tee -> [speaker, HiFi4] at 16000 Hz
   <inf> mpipe_ipc_infer: [1] heard "yes"   (0 buffers dropped)
   <inf> mpipe_ipc_infer: [2] heard "no"   (0 buffers dropped)

The HiFi4 has no console on this board -- both cores' consoles are on ``uart4``
and the DSP overlay gives it up -- so its side is visible through the counters
it publishes in the shared window and the categories it reports upstream.
