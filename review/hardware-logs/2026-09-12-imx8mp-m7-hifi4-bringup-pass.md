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

**Open, minor:** the sample's round-trip counter advances more slowly than
heartbeats are sent while reporting success and logging no error (10, 16, 16
across 30 sends). The discrepancy is in the sample's instrumentation, not in the
transport -- the link itself keeps working and recovers -- but it is not yet
explained.
