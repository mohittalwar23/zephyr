.. zephyr:code-sample:: mpipe-ipc-bringup
   :name: mpipe direct M7 to HiFi4 IPC bring-up

   Bring up a direct Cortex-M7 to HiFi4 link over MU3 and exchange control
   messages, with no relay through Linux.

Overview
********

Both cores run the same source; only the board overlay differs. The sample
establishes a session, opens an IPC Service instance over MU3, binds an
endpoint, and then runs both halves of the split this design is built around:

* **Control** messages -- heartbeats and status reports -- travel over MU3
  through IPC Service.
* **Audio** travels through a shared-DDR ring and never touches the message
  path, following the same split the NXP SDK, NXP's Linux side and SOF all use.

The M7 produces periods stamped with their own sequence number; the HiFi4
verifies every byte and reports what it saw back over the control path. Either
core can be restarted underneath the other and both the link and the stream
rebuild themselves.

Memory
******

The reserved window is 256 KiB at ``0xa0000000``:

=============  ========  =====================================================
Address        Size      Contents
=============  ========  =====================================================
``0xa0000000``    4 KiB  Bring-up control block: session, state, last error
``0xa0010000``   64 KiB  IPC Service shared memory (vrings and buffers)
``0xa0020000``  128 KiB  PCM ring: 32 periods of 640 bytes
=============  ========  =====================================================

Every region is a power-of-two size at a naturally aligned base. The M7's
ARMv7-M MPU can express nothing else -- it rounds a size up to the next power of
two and masks the base to match -- so a region that is neither silently covers
something other than what was asked for.

Building and running
********************

The i.MX8M Plus EVK needs a devicetree that reserves the shared window, and a
specific start order. Both are consequences of the hardware, not conveniences.

**The shared window must be reserved.** Add a ``no-map`` reservation at
``0xa0000000`` of 256 KiB to the Linux devicetree. Without it Linux treats that
memory as ordinary RAM and the sample corrupts it.

**MU3 is powered by the DSP.** MU3 sits in AUDIOMIX and is clocked as a
peripheral clock of the DSP node, so it runs only while Linux holds the DSP
runtime-resumed. Two consequences:

* The host must not configure MU3 before the DSP exists. The sample calls
  :c:func:`mpipe_ipc_transport_require_peer`, so the M7 waits rather than
  writing to an unclocked mailbox -- those writes are discarded silently, and
  the result is a link that comes up, agrees on everything, and never delivers
  a single doorbell to the host.
* Tearing down after the DSP stops still touches MU3. Hold the clock across
  that window::

    # start: let runtime PM manage the clock
    echo auto > /sys/devices/platform/3b6e8000.dsp/power/control
    echo start > /sys/class/remoteproc/remoteproc0/state   # M7, waits for the DSP
    echo start > /sys/class/remoteproc/remoteproc1/state   # DSP

    # before stopping the DSP, hold MU3 alive for the M7
    echo on   > /sys/devices/platform/3b6e8000.dsp/power/control
    echo stop > /sys/class/remoteproc/remoteproc1/state

    # release the hold again before restarting the DSP
    echo auto > /sys/devices/platform/3b6e8000.dsp/power/control
    echo start > /sys/class/remoteproc/remoteproc1/state

  The release is required: with the hold in place the DSP does not complete its
  firmware-ready handshake and ``start`` fails with ``-ETIMEDOUT``.

**Only one core gets a console.** Both cores' consoles are on ``uart4``, so the
DSP overlay disables it and leaves the port to the M7. The DSP's state is
readable from Linux instead: each core publishes its session, its bring-up
state, and the reason it last went down into the reserved window, which
``/dev/mem`` can read at ``0xa0000000``.

Sample output
*************

.. code-block:: console

   *** Booting Zephyr OS build v4.4.0 ***
   <inf> mpipe_ipc_bringup: mpipe IPC bring-up: role=host shared=0xa0000000
   <inf> mpipe_ipc_bringup: gen 0: session 4 (peer word 0x00030002)
   <inf> mpipe_ipc_bringup: gen 0: LINK UP after 128 polls: session 4 <-> 3
   <inf> mpipe_ipc_bringup: gen 0: endpoint 'mpipe.ctrl' bound
   <inf> mpipe_ipc_bringup: gen 0: FIRST ROUND TRIP: heartbeat 0 acknowledged in 4 us
   <wrn> mpipe_ipc_bringup: gen 0: peer restarted; standing down so it can rebuild
   <inf> mpipe_ipc_bringup: gen 0: ring up: 32 periods of 640 bytes
   <inf> mpipe_ipc_bringup: gen 0: produced 208 periods, fill 16; remote verified 0, 0 corrupt
   <inf> mpipe_ipc_bringup: gen 0: produced 400 periods, fill 16; remote verified 208, 0 corrupt
   <wrn> mpipe_ipc_bringup: gen 0: peer restarted; standing down so it can rebuild
   <inf> mpipe_ipc_bringup: gen 1: LINK UP after 1 polls: session 10 <-> 14
   <inf> mpipe_ipc_bringup: gen 1: ring up: 32 periods of 640 bytes
