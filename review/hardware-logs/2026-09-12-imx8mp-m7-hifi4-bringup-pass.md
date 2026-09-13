# i.MX8MP M7 <-> HiFi4 direct IPC: first working two-core bring-up

Date: 2026-09-12
Board: i.MX8M Plus EVK, probe DTB d5d87c41 (reserves ipc@a0000000 no-map)
Transport: MU3 (MU3_A IRQ138 on M7, MU3_B CLIC IRQ7 on HiFi4)
Backend: ipc_rpmsg_static_vrings, M7 = host, HiFi4 = remote
Control block: 0xa0000000 (4 KiB), IPC shm: 0xa0001000 (252 KiB)

## Result: all four cases pass

| case                              | outcome                                       |
|-----------------------------------|-----------------------------------------------|
| cold boot, host first             | both READY, mutual acknowledgement, error=0   |
| cold boot, remote first           | remote waits, host opens, both READY          |
| restart the remote x2             | host keeps its session; both READY each time  |
| restart the host under live remote| host waits for the remote to stand down first |

The last row is the barrier working on silicon: the returning host held off
for 4 polls until the live remote published DOWN, then opened and attached to
that remote's existing session rather than wiping the rings underneath it.

## Five defects the board found that no unit test had

1. **CONFIG_HEAP_MEM_POOL_SIZE=0 -> -ENOMEM from open.**
   `ipc_static_vrings_init()` calls `virtqueue_allocate()` once per ring and
   that reaches `k_malloc()`. static-vrings is not heap-free -- an earlier
   note claiming it was is corrected. Builds and links clean; only the board
   catches it.

2. **The transport masked backend errors as -ECONNRESET.** A local
   misconfiguration and a peer restart demand opposite responses, and the
   console said "peer reset" for what was actually -ENOMEM.

3. **Residue is not a peer.** The host latched a session word left behind by
   the remote's previous life, then read the remote's real boot as a restart
   and stood down. Fixed by the rule that a peer is accepted only once it
   acknowledges *this boot's* session -- which residue can never do, because
   `mpipe_ipc_session_next()` skips whatever the peer last acknowledged.
   A READY word nobody answers for is broken by a bounded timeout.

4. **quiesce() skipped the close it existed to do.** It closed only while
   RUNNING, but detecting a peer restart leaves the transport FAULTED with
   the instance still open -- the one case that needs the close. The next
   open then failed -EALREADY and the link never came back. "Link running"
   and "instance open" are now tracked separately.

5. **Recovery looked like a restart.** Re-initialising after a peer restart
   claimed a new session, which the peer could not distinguish from this core
   rebooting, so it faulted and rebuilt too -- and the two chased each other.
   Split into `rebuild()` (the peer restarted) and `init()` (this core did).

Also added: each core publishes its failure reason as a negative errno in the
shared block. Both cores share one UART on this board, so at most one has a
console and in a product neither does; without it a remote that failed to open
is indistinguishable from one that never started.

## Console, case C (host restart under a live remote)

[00:00:22.404,000] [0m<inf> mpipe_ipc_bringup: gen 2: LINK UP after 1 polls: session 19 <-> 17[0m
*** Booting Zephyr OS build v4.4.0-12602-g4dce5cba3c7a ***
[00:00:00.000,000] [0m<inf> mpipe_ipc_bringup: mpipe IPC bring-up: role=host shared=0xa0000000[0m
[00:00:00.000,000] [0m<inf> mpipe_ipc_bringup: gen 0: session 20 (peer word 0x00130011)[0m
[00:00:00.060,000] [0m<inf> mpipe_ipc_bringup: gen 0: LINK UP after 4 polls: session 20 <-> 17[0m

## Final control block

  host/M7     session=0x00110014 req=20    ack=17    state=2 READY    error=0
  remote/DSP  session=0x00140011 req=17    ack=20    state=2 READY    error=0

---

## Addendum, same day: first data across the link

The bring-up above established the rings but registered no endpoint, so not one
byte had actually crossed MU3. Adding an endpoint and a heartbeat round trip
exposed a sixth defect, and this one was invisible to every layer above it.

**MU3 is powered by the peer.** It sits in AUDIOMIX and is clocked as a
peripheral clock of the DSP node (`mu3_cg` -> `3b6e8000.dsp`, `per_clk1`), so it
runs only while Linux holds the DSP runtime-resumed. Measured directly:

    DSP suspended -> mu3_cg enable_count 0
    DSP resumed   -> mu3_cg enable_count 1

The M7 starts first, so it was configuring MU3_A -- `MU_Init()`, then the
receive-interrupt enables -- while the block was ungated. Those writes are
discarded with no error reported. The result was a link that passed every check
and ran exactly one way:

    M7  -> DSP   21 of 21 doorbells delivered
    DSP -> M7     0 of 26 doorbells delivered

Everything downstream followed from that. The DSP's name-service announcement
was sitting in the host's receive ring the whole time -- readable from Linux at
0xa0001004 as `6d706970652e6374726c` ("mpipe.ctrl"), addressed to dst 0x35, the
RPMsg name-service address. It had been sent correctly. The host simply never
got the interrupt telling it to look, so no endpoint ever bound and no data
moved. Bring-up itself never noticed, because it is deliberately poll-only and
needs no doorbell.

Confirmed by holding the clock on before starting the M7: `received` went from
0 to 21.

**Fix:** `mpipe_ipc_transport_require_peer()`. The host waits for the peer to
acknowledge it before opening the instance. On this SoC "the mailbox is clocked"
and "the peer is alive" are the same condition, so the barrier already had the
information -- it just was not being used. This costs a host nothing, since it
has nobody to talk to until the peer exists.

It does not cover teardown: standing down after the DSP stops still writes to
MU3. That window needs the runtime-PM hold, and the hold conflicts with starting
the DSP (`start` then fails `-ETIMEDOUT`), so it must be released first. The
sequence is in the sample README.

## Result

    gen 0: LINK UP after 128 polls: session 4 <-> 3
    gen 0: endpoint 'mpipe.ctrl' bound
    gen 0: FIRST ROUND TRIP: heartbeat 0 acknowledged in 4 us
    gen 0: peer restarted; standing down so it can rebuild
    gen 1: LINK UP after 1 polls: session 4 <-> 4
    gen 1: endpoint 'mpipe.ctrl' bound
    gen 1: FIRST ROUND TRIP: heartbeat 34 acknowledged in 4 us

Control messages cross MU3 in 4 us, verified against the peer's session on every
receive, and the whole path -- including the endpoint -- rebuilds itself after
the remote restarts. 270 consecutive round trips with 0 dropped in one run.

**The counter discrepancy was a real bug, not instrumentation.**
`ipc_service_send()` returns the *number of bytes sent* on success, not zero.
`send_command()` passed that straight back, and the caller treated a non-zero
return as "done" -- so every send returned 4, the four header bytes, which was
then printed as a 4 us round-trip time. The reply was never waited for at all.

With the peer stopped this reported eighteen consecutive successful 4 us round
trips into a dead core, with no error and no timeout. Corrected:

    before:  hb 16 -> 4 us, trips=15     (peer already gone)
    after:   heartbeat 11 failed: -116   (ETIMEDOUT, honestly)

Real round-trip time on this link is **23-26 us**, and the trip counter now
tracks sends one for one. The lesson generalises past this sample: a Zephyr API
that returns a count on success cannot be tested with `!= 0`, and here the wrong
test produced a number plausible enough to be mistaken for a measurement.

---

## Addendum: PCM across the shared ring

Bulk audio now moves through a dedicated shared-DDR ring, off the message path,
with control messages on MU3 alongside it. Both halves run at once.

Layout inside the existing 256 KiB reservation, no Linux devicetree change:

    0xa0000000    4 KiB   bring-up control block
    0xa0010000   64 KiB   IPC Service shared memory
    0xa0020000  128 KiB   PCM ring, 32 periods of 640 bytes

Every region is a power-of-two size at a naturally aligned base, because the
M7's ARMv7-M MPU rounds a size up to the next power of two and masks the base to
match. The previous 252 KiB region at 0xa0001000 was neither, and only happened
to work because the rounding landed on a superset of what was intended.

**Ring design.** Single producer, single consumer; each side writes one sequence
counter the other only reads, which is the same lock-free argument the bring-up
control block rests on. The counters are monotonic rather than wrapped indices,
so `write - read` is the fill directly and is correct across the wrap without
sacrificing a slot. Claim/commit rather than copy, so a DMA engine can write
straight into a period.

**The producer publishes its geometry and the consumer checks it.** A period
size or count mismatch is silent in hardware -- each side simply addresses
different bytes than the other wrote. The same class of silent disagreement in
the mailbox channel mapping had already cost a long session on this link.

## Result

    gen 0: ring up: 32 periods of 640 bytes
    gen 0: produced 208 periods, fill 16; remote verified 0, 0 corrupt
    gen 0: produced 400 periods, fill 16; remote verified 208, 0 corrupt
    gen 0: produced 608 periods, fill 16; remote verified 400, 0 corrupt
    gen 0: produced 800 periods, fill 16; remote verified 608, 0 corrupt
    gen 0: peer restarted; standing down so it can rebuild
    gen 1: ring up: 32 periods of 640 bytes
    gen 1: produced 800 periods, fill 16; remote verified 608, 0 corrupt

    producer seq=928 period=640B count=32 overruns=0
    consumer seq=928 underruns=1

Every period is stamped with its own sequence number, so the consumer catches a
period delivered late, twice, or out of order -- which a constant pattern would
not. Zero corrupt periods across both generations, zero overruns, and the single
underrun is the consumer arriving before the producer had filled anything.

**One reporting bug found and fixed here**, of the same family as the byte-count
one: progress was reported on `periods % 200 == 0`, but periods advance in
bursts of up to 16, so the exact multiples were stepped over and the remote's
verified count appeared frozen at 400. Cross a threshold, do not land on a
multiple.

---

## Addendum 2026-09-13: keyword spotting over the link

TFLM now runs on the HiFi4 and the full path works end to end.

**TFLM on HiFi4, standalone.** tflite-micro's own `micro_speech` test clips,
classified on silicon:

    clip 'yes'     -> 'yes'     CORRECT (52704 us)
    clip 'no'      -> 'no'      CORRECT (52600 us)
    clip 'silence' -> 'silence' CORRECT (52586 us)
    3 of 3 clips classified correctly

About 52.6 ms per second of audio, or 5.3% of real time. That is with the
reference kernels: CMSIS-NN is gated `if CPU_CORTEX_M`, so the HiFi4 has no
accelerated NN kernels today. Wiring up xa_nnlib is a separate piece of work and
this is the number to beat.

**Over the link.** The M7 streams one second of 16 kHz mono per window through
the shared ring, alternating "yes" and "no"; the HiFi4 rebuilds each window,
runs micro_speech, and reports the category back over the control endpoint.

    gen 0: ring up, 64 x 320 bytes
    gen 0: 195 windows correct, 0 wrong

    producer seq=19848 period=320B count=64 overruns=0
    consumer seq=19808 underruns=0

Alternating two spoken words is deliberate. A single word on repeat would prove
only that the remote keeps answering -- a classifier that had stopped listening
would score identically. Two words means a wrong answer shows up.

**Findings**

* **PR #96657 hardcodes `$ENV{ZEPHYR_BASE}/../optional/...`** for the TFLM
  signal sources while using `ZEPHYR_TFLITE_MICRO_MODULE_DIR` for the matching
  include paths. The hardcoded form breaks in any layout where the workspace is
  not the parent of ZEPHYR_BASE -- a git worktree, for instance. Worth sending
  upstream.
* **`micro_speech_process_audio()` returns a status, not a category**, and logs
  the label internally, so a caller cannot act on the classification. Also worth
  sending upstream; a pipeline stage has to act on a result, not read it in a
  log.
* **The M7 cannot hold three seconds of 16-bit PCM.** Its read-only data lives
  in ITCM; the third clip overflowed the region by 12,380 bytes. Two clips.
* The inference sources are **vendored** into `samples/subsys/mpipe/ipc_infer/
  src/inference/` with provenance headers, because the two changes above are
  needed and #96657 is still open. That copy should be deleted once it merges.

---

## Addendum 2026-09-13: live capture, and a duplication caught

Live audio now reaches the DSP and comes back classified. Two capture paths
work, and one architectural mistake was found and is being reversed.

**Live capture works both ways in.**

    capture pipeline: i2s -> tee -> [speaker, HiFi4] at 16000 Hz
    forwarding 16000 Hz, 2 ch, 16-bit as 160-sample mono periods
    codec route is now 1 ... now 2
    TX started with 3 silence buffers
    Capture started now
    [2] heard "yes"   (0 periods dropped)

The tee gives an audible branch alongside the inference branch, which is the
check that fails first: keyword spotting only tells you whether the model
agreed, while hearing the same audio tells you whether the microphone, clocks
and gain are doing anything at all.

**DMIC's apparent failure was ours, not the hardware's.** `dmic_read()` with a
zero timeout finds the driver's queue momentarily empty and takes a fallback
that returns *silence and success*. A polling caller therefore never blocks,
never fails, and fills the ring with zeros as fast as it can loop. The proven
path in `mpipe_aud_dmic_src` uses a bounded read (~100 ms) on its own thread.
With that fixed the level went from 0 to real room noise and windows moved from
15 per second to exactly one per second, which is the signature of capture
paced by the microphone rather than by a spin.

**The M7 DDR board variant does not run here.** With the pipeline the TCM build
overflowed ITCM by 4728 bytes, so `imx8mp_evk/mimx8ml8/m7/ddr` was tried.
It builds, `imx_rproc` reports booting it, and the M7 then re-executes whatever
is still resident in ITCM -- caught only by checking which log strings were in
which ELF. `CONFIG_LOG_MODE_MINIMAL=y` makes the TCM build fit instead.

**The duplication.** A `mpipe_ipc_sink` element was written to feed the ring.
It should not have been: PR #114088's plugin (`c2c21ff684c` on
`gsoc/mp-ipc-plugin`) already contains `mp_ipc_sink.c`, `mp_ipc_src.c` and
`mp_ipc_translation.c` -- and is *more* complete, carrying the consumer-side
element, buffer-release accounting, state propagation, properties, events and
queries.

It is also already zero-copy, which was the assumption that went unchecked. It
does not put samples in messages; it passes a descriptor:

    struct { uint32_t phys_addr; uint32_t size; uint32_t timestamp;
             uint32_t buffer_id; } data;

The sink refs the net_buf, flushes cache, sends the descriptor, and the consumer
reads the samples from that address and returns a RELEASE carrying the
buffer_id. Audio never travels in the payload -- exactly the property the ring
was built to obtain.

    Devanshi's                         this work's ring
    samples by reference               samples in a ring
    2 messages per buffer              0 messages per buffer
    mp_ipc_src element                 hand-rolled consumer
    no peer-restart handling           session + barrier

**Decision: converge on the plugin, drop `mpipe_ipc_sink` and the ring.** Her
data path is equivalent and is the upstream-track work; the session and barrier
layer stays underneath it, since that solves the restart problem the plugin does
not address and is the part of this work that is actually new.

Known integration point: `phys_addr` is the buffer's own address, so the audio
pool must sit in memory both cores can address. The M7's pool is in TCM today,
which the HiFi4 cannot see, so it has to move into the shared DDR window.

---

## Addendum 2026-09-13: one pipeline across two cores

The pipeline now spans the link. Captured on the M7, inferred on the HiFi4,
with an audible branch in between:

    i2s_src -> caps_filter -> tee -+-> gain -> i2s_codec_sink   (audible)
                                   +-> ipc_sink ==MU3==> ipc_src -> micro_speech

Running, read out of shared memory because the DSP has no console:

    DSP: buffers=1056 windows=10 last='no' refused=0 bound=1 playing=1
    DSP: buffers=2075 windows=20 last='no' refused=0 bound=1 playing=1

2075 buffers in 22 s is 94 a second against 100 expected for 10 ms periods, and
20 windows in 22 s is one inference per second. Nothing refused, nothing
starved.

## Root cause of the stall

Two faults compounded, and the second hid the first.

**The pipeline only knew how to pull.** `mpipe_pipeline.c` finds the first
source, requires `acquire_buffer`, and calls it in a loop, treating a refusal as
a flow error that stops everything. An IPC source cannot answer that: its
buffers arrive when the peer sends them. Sources now declare a drive model --
`MPIPE_SRC_DRIVE_PULL`, the default, or `MPIPE_SRC_DRIVE_PUSH` -- and a pushed
source is not polled. This is not specific to IPC; a network source has the same
shape.

**Nothing was sized for a branching, cross-core pipeline.**
`CONFIG_MPIPE_NET_BUF_POOL_COUNT` defaults to **1**, which is enough only for a
pipeline that holds one buffer at a time. A tee references the same buffer on
both branches at once, and the IPC branch then holds it for a whole round trip
to the DSP and back. On top of that the sink's outstanding window was a fixed 16
against a 12-buffer pool -- the sink could take every buffer in existence and
the source that fills them could never allocate another.

The outstanding window is now `CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS`, documented
as a claim on the upstream pool, and the sample sizes its pools for the real
demand.

## Three faults found before those, each fatal

  - **The endpoint configuration was a stack local.** IPC Service stores a
    pointer to it, not a copy, so the first callback after registration jumped
    through whatever had replaced it. It presents as an illegal instruction at
    a nonsense PC inside the backend's bound handler, which is only traceable by
    resolving the return address -- the PC itself is garbage.
  - **The source's wrapper pool passed no destroy callback**, so a refcount
    reaching zero never reached the pool's release hook and no buffer would ever
    have gone back to the peer.
  - **Registering the endpoint is what makes the peer start sending**, and that
    happens before the rest of the pipeline is built. Buffers arriving in that
    window were pushed into an unlinked pad.

## Two traps worth recording

  - **Do not link into a shared window.** Placing the pool there with a section
    attribute gives the image a load segment covering memory it does not own,
    and `imx_rproc` then refuses to start the core with a bare `-EINVAL`. The
    region is addressed, never linked into; `zephyr,mpipe-aud-pool` says where.
  - **Whoever owns the console owns the pin mux.** Building the M7 quiet so the
    DSP could talk left nobody applying uart4's pinctrl, and a DSP that boots
    and says nothing looks exactly like a DSP that crashed early. Separately,
    the host's USB serial re-enumerated mid-session and several "no output"
    runs were that, not the target. Shared-memory telemetry is the reliable
    channel for a headless core.

---

## Addendum 2026-09-13: the plugin's other half, and a lossy release

The query, event and caps paths are ported, the micro_speech duplication is
gone, and a leak that had been hiding as a throughput problem is fixed.

**One negotiation now covers both halves.** The sink announces the format its
pipeline settled on and the source applies it, instead of each core being
configured separately and trusted to match. A format two cores are each *told*
is a format they can silently disagree about: every element accepts and the
audio is simply wrong. End of stream crosses the same way.

Both are added by extending the base sink's handlers rather than replacing
them. Replacing `event_fn` lost the pad's own caps bookkeeping and the bus
message that tells a pipeline its stream ended -- visible as throughput
collapsing to a twelfth of the rate, not as anything resembling a missing
event.

**Releases were lossy, and that reads as a throughput problem.** The control
channel refuses a send when its transmit buffers are momentarily exhausted,
which under load is ordinary. The release was dropped there, and a dropped
release is one of the peer's slots leaked permanently. Enough of them starve
the pipeline feeding the link, slowly:

    before   M7 dropped 0 -> 457 in 10 s, delivery 54/s, one window per 12 s
    after    M7 dropped 0,               delivery 100.6/s, one window per second

100.6 buffers a second against 100 expected for 10 ms periods, and exactly one
inference per second. Failed releases are now remembered and retried, and a
push downstream that is refused drops its reference, which releases the buffer
rather than stranding the slot the same way.

**The duplication is gone.** micro_speech is cherry-picked to its own upstream
path with its author's commits intact, this sample references it, and the three
changes it needs sit in one commit that can be sent to #96657: return the
category rather than log it, stop including the sample's rpmsg transport from
the inference code, and find the TFLM signal sources through
ZEPHYR_TFLITE_MICRO_MODULE_DIR rather than a path relative to ZEPHYR_BASE that
is wrong in a git worktree. 2873 lines of copy removed.

**Host note:** the USB serial bridge stopped delivering mid-session -- the
device enumerates, reads return nothing. Several "the core printed nothing"
runs were that and not the target. Both cores now publish progress into the
reserved window, which is the channel that does not depend on a host cable.
