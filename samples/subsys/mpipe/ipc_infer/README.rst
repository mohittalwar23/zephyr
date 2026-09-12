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

``src/inference/`` is a copy of the micro_speech inference code from Zephyr
pull request #96657, *"samples: tflite-micro: add micro_speech application with
OpenAMP on i.MX8MP"*, which is still open. It is copied rather than referenced
because this sample needs one change that the pull request does not have:

* ``micro_speech_process_audio()`` upstream returns 0, -1 or -2 and logs the
  detected label internally, so a caller cannot act on the result. Here it
  returns the category index, and ``micro_speech_category_label()`` names it.
  A pipeline stage has to act on a classification, not read it in a log.

The copy also drops ``#include "transport/rpmsg_transport.h"`` from
``model_runner.cpp``: upstream couples inference to its Linux transport, and
this link replaces that transport entirely.

**Delete this copy and depend on the upstream sample once #96657 merges.** Both
changes above are worth sending to that pull request.

The audio clips in ``src/test_clips.c`` are generated from tflite-micro's own
``micro_speech`` test data by ``scripts/make_test_clips.py``; that script records
where they came from and regenerates them.

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
