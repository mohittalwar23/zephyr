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
