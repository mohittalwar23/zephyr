.. zephyr:code-sample:: mpipe-ipc-infer
   :name: mpipe keyword spotting across a direct M7 to HiFi4 link

   Stream audio from the Cortex-M7 to the HiFi4 over a shared ring and run
   micro_speech keyword spotting on it, with no relay through Linux.

Overview
********

This is the end-to-end path the direct link exists for, with the microphone
still stubbed:

.. code-block:: none

   clip -> M7 -> shared DDR ring -> HiFi4 -> micro_speech -> result -> M7

Both images are built from the same source; only the board overlay differs, and
only the DSP builds inference. The M7 sends one second of 16 kHz mono audio per
window, alternating "yes" and "no". The HiFi4 rebuilds each window from the ring,
runs micro_speech over it, and reports the category back over the control
endpoint. The M7 knows which word it sent, so a wrong answer is counted rather
than merely logged.

Sending a single word on repeat would prove only that the remote keeps
answering. Alternating two spoken words means a classifier that has stopped
listening cannot score by accident.

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

The audio clips in ``src/test_clips.c`` are generated from tflite-micro's own
``micro_speech`` test data by ``scripts/make_test_clips.py``.

Requirements
************

The optional ``tflite-micro`` module, which is not fetched by default::

   west update tflite-micro

Everything the bring-up sample requires applies here too -- the reserved shared
window, the start order, and the MU3 clock's dependence on the DSP being
runtime-resumed. See :zephyr:code-sample:`mpipe-ipc-bringup`.

Memory
******

Same 256 KiB reservation at ``0xa0000000`` as the bring-up sample, with the ring
carrying mono rather than stereo periods:

=============  ========  =====================================================
Address        Size      Contents
=============  ========  =====================================================
``0xa0000000``    4 KiB  Bring-up control block
``0xa0010000``   64 KiB  IPC Service shared memory
``0xa0020000``  128 KiB  PCM ring: 64 periods of 320 bytes
=============  ========  =====================================================

A period is 160 samples, 10 ms at 16 kHz mono, so one inference window is
exactly 100 periods. micro_speech wants mono, which is why this sample's ring
geometry differs from the bring-up sample's stereo one -- and why the consumer
checks the producer's published geometry before attaching.

Only two clips are carried, not three: the M7's read-only data lives in ITCM,
which cannot hold a third second of 16-bit PCM.

Sample output
*************

On the M7::

   *** Booting Zephyr OS build v4.4.0 ***
   <inf> mpipe_ipc_infer: mpipe IPC inference: role=host
   <inf> mpipe_ipc_infer: gen 0: link up, session 2 <-> 1
   <inf> mpipe_ipc_infer: gen 0: endpoint bound
   <inf> mpipe_ipc_infer: gen 0: ring up, 64 x 320 bytes
   <inf> mpipe_ipc_infer: gen 0: 30 windows correct, 0 wrong
   <inf> mpipe_ipc_infer: gen 0: 195 windows correct, 0 wrong

The HiFi4 has no console on this board -- both cores' consoles are on ``uart4``
and the DSP overlay gives it up -- so its side is visible through the counters
it publishes in the shared window and the categories it reports upstream.
