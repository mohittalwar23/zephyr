# i.MX8MP M7-to-HiFi4 Direct IPC Design

Date: 2026-09-11

Status: Design approved; implementation not authorized

Target board: NXP i.MX8M Plus EVK

Implementation base: `a730906a1f6d986a159966f5a488560e7ba4dd03`

Shared NXP HAL under test: `1123e43d6350489036f5a14d296c2c18cbd39478`

## Authority and scope

This specification turns the approved direction in the IPC M7/HiFi4 build
handoff into an implementation-ready design. Linux owns lifecycle management
of both remote cores. Real-time audio travels directly from the Cortex-M7 to
the HiFi4 over MU3 notifications and a dedicated shared-DDR static-vrings
transport.

The authoritative inputs are:

- `/home/mt/Documents/Codex/2026-09-08/re/outputs/IPC-M7-HIFI4-BUILD-HANDOFF-2026-09-10.md`, SHA-256
  `8b81fdf91c36420477b1b4a143e6ef3d04bf12c93466f6bab7fcaf56e15f87ef`.
- `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-m7-hifi4-precode-audit.log`, the read-only source and live-board audit.
- NXP i.MX 8M Plus Applications Processor Reference Manual, Rev. 3,
  August 2024.

This document is a design artifact only. It does not authorize source changes,
firmware installation, remote-processor starts, device-tree replacement, or a
push. Tested branches and commit attribution must remain intact.

## Goals

- Keep Linux remoteproc as the sole lifecycle authority for both the M7 and
  HiFi4.
- Carry low-latency audio directly between the remote cores, without routing
  each audio frame through Linux.
- Use one MU3-backed static-vrings transport with explicit ownership,
  backpressure, validation, and recovery rules.
- Integrate the direct link with the existing M7 mpipe capture path and the
  HiFi4 micro-speech input path.
- Make firmware provenance, memory placement, and runtime behavior observable
  and reproducible.

## Non-goals

- Linux is not an audio data-plane relay.
- This version does not use raw shared-memory polling for audio.
- This version does not support independent restart of one remote while the
  other continues streaming.
- This version does not add Linux suspend/resume support while the pair is
  running.
- This version does not replace Linux remoteproc management with remote-core
  self-boot or mutual boot.
- This version does not upstream unfinished or unattributed experimental work.

## Current state and handoff deltas

The pre-code audit confirmed most of the handoff and found these material
differences or refinements:

| Area | Handoff starting point | Verified current state | Design consequence |
| --- | --- | --- | --- |
| Linux DT topology | Prior M7 and DSP images were described as mutually exclusive | The running `imx8mp-evk-rpmsg.dtb` exposes both remoteproc nodes simultaneously; both are currently offline | A combined remoteproc base already exists. SOF remains a mutually exclusive runtime stack, but generic M7 and DSP remoteproc coexistence does not need to be invented |
| Kernel identity | Short BSP identity was known | Running kernel is `6.6.52-lts-next-g90192c5d29cb`; exact commit is `90192c5d29cb650fd7f7dd9094af14eefb38837d` | Design must accommodate the exact 6.6 DSP driver's firmware-ready wait behavior |
| Shared DDR placement | Address was open | Live memory has an unreserved 19 MiB gap at `0x91100000-0x923fffff` | Reserve a provisional 128 KiB window at `0x91100000`; final acceptance remains gated on clean firmware ELF checks and a hardware probe |
| Linux audio nodes | Not fully inventoried | RPMsg audio and RPMsg MICFIL nodes are enabled; physical MICFIL, SAI3, SDMA3, and sound nodes are disabled | The direct-MU3 production overlay must disable both Linux RPMsg audio nodes to avoid stale ownership and misleading devices |
| MU3 runtime state | Register permissions and clock state were unknown | With both remotes offline, `mu3_cg` is disabled and AudioMix is powered off; strict `/dev/mem` prevents read-only RDC inspection | MU3 clock/power and RDC permissions require a provenance-clean probe; no conclusion may be inferred from offline state |
| Installed experiments | Old binaries were known to exist | `mu3-*`, `xcore-*`, and related Zephyr artifacts identify prohibited commit `69cb3b08a12a`; none was run | Installed experiments are quarantined and cannot serve as evidence or implementation input |
| Upstream Zephyr | Moving baseline was expected | `origin/main` advanced to `3860b8cb663...`; scoped IPC/MBOX/i.MX/mpipe review found no relevant replacement for the tested integration | Continue from the exact tested integration, not a moving rebase |

## Verified platform facts

### Audited source and board identities

- The isolated design branch starts exactly at tested EVK integration commit
  `a730906a1f6d986a159966f5a488560e7ba4dd03`.
- The shared NXP HAL is detached at
  `1123e43d6350489036f5a14d296c2c18cbd39478`; this is newer than the
  integration manifest pin and is part of the tested hardware state.
- The SOF reference tree is clean at
  `3303a6ccd40833ffeda4d5e903bcb1751a154da3` on `dai-burst-fix`.
- The active board DTB has SHA-256
  `755dd0e66b7258c3124354978ad240e163c3258309d5184fb3227b8189dab90d`.
- The board audit was read-only: neither remote processor was started and no
  target file, boot artifact, or device-tree blob was modified.

### Remote processors and existing management links

- Linux remoteproc0 is the M7 (`imx-rproc`) and currently uses MU1.
- Linux remoteproc1 is the HiFi4 (`imx-dsp-rproc`) and currently uses MU2.
- Both are offline in the audited state.
- The running DSP driver requests MU2 transmit, receive, and receive-doorbell
  channels and waits for a firmware-ready notification during start.
- The exact 6.6 driver has a `no_mailboxes` diagnostic module parameter, but
  its suspend path assumes a transmit channel. It is not a production solution.
- A later NXP driver supports the vendor `DONT_WAIT` resource-table flag, but
  that behavior is not present in the running kernel.

### MU3 hardware resources

- M7 view, MU3_A: `0x30e80000-0x30e8ffff`, IRQ 138, RDC PDAP18,
  SEMA42 gate 18.
- HiFi4 view, MU3_B: `0x30e90000-0x30e9ffff`, DSP IRQ 7, RDC PDAP31,
  SEMA42 gate 31.
- AudioMix clock-enable register `CLKEN1` is at offset `0x4`; bit 5 enables
  MU3.
- DSP IRQ 7 combines MU2 and MU3 interrupt sources.

### Zephyr mailbox and static-vrings constraints

- New MU3 device-tree nodes must bind to `nxp,mbox-imx-mu`.
- The current NXP mailbox driver's enable/disable operation affects all four
  generic receive channels for an instance, not just the requested channel.
- Every enabled mailbox instance connects its device to the instance IRQ.
  Creating separate DSP MU2 and MU3 mailbox devices on shared DSP IRQ 7 is
  therefore unsafe with the current driver.
- A static-vrings instance shares one vring pool among its logical endpoints.
  Independently closing two instances that share MU channels is unsafe because
  one close can disable receive interrupts used by the other.
- The backend defers received work from the mailbox callback, supports
  hold/release of receive buffers, and supports nonblocking or forever-waiting
  transmit acquisition only.
- Transmit-buffer rollback is unsupported after a buffer is claimed. All work
  that can fail must finish before buffer acquisition; a subsequent transport
  send failure is fatal to the session.

## Architecture

The system has a Linux lifecycle plane and a remote-to-remote data plane:

```text
                          lifecycle / boot readiness
                 MU1                              MU2
    Linux remoteproc <----------> M7     Linux remoteproc <----------> HiFi4
                                      <----- one-shot FW_READY

                               real-time data plane
                     MU3 notifications + 128 KiB shared DDR
                  M7 host  <==========================>  HiFi4 remote
                       one static-vrings physical instance
                         control endpoint + audio endpoint
```

Linux starts, stops, and recovers the pair. Linux does not inspect or forward
normal audio frames. The M7 is the static-vrings host and the HiFi4 is the
remote. MU3 is dedicated to this direct transport.

## Linux lifecycle and the MU2 bootstrap exception

The normal start order is HiFi4 first, then M7. This allows the DSP to complete
its Linux remoteproc handshake and bind the direct endpoint before capture can
start. The normal stop order is M7 capture stop and drain, M7 stop, then HiFi4
stop.

On the exact 6.6 BSP, the HiFi4 firmware sends exactly one raw, transmit-only
MU2 firmware-ready doorbell during boot. It does not instantiate a Zephyr MU2
mailbox interrupt device and does not use MU2 for application data. This narrow
exception satisfies the Linux driver's existing boot contract without creating
the shared-IRQ collision between Zephyr MU2 and MU3 devices.

System suspend is prohibited while either member of the pair is running. The
orchestrator must stop the pair before suspend. A future BSP backport of the
vendor `DONT_WAIT` behavior, paired with a resource table that does not create a
phantom RPMsg vdev, can remove the one-shot exception after a separate review.

No remote may unilaterally reboot itself. A transport-fatal, protocol-fatal, or
remoteproc crash transitions the session to fault and asks Linux orchestration
to stop and restart both cores.

## Device-tree ownership

The production Linux device tree must:

- keep both remoteproc nodes enabled and retain their established management
  carveouts;
- reserve the direct shared-memory window as a root `reserved-memory` child
  with `no-map`;
- omit the direct window from both remoteproc `memory-region` arrays so neither
  driver's prepare phase clears live peer data;
- disable the existing RPMsg audio and RPMsg MICFIL nodes for this operating
  mode; and
- leave physical audio peripherals assigned according to the selected M7
  capture topology.

The M7 Zephyr device tree gains MU3_A at its verified address and IRQ. Its
existing MU1 management path remains separate. The HiFi4 Zephyr device tree
gains only the MU3_B mailbox interrupt device; it must not create a MU2 mailbox
interrupt device. The boot-only MU2 ready write is a narrowly isolated platform
operation.

Mailbox channel direction must be complementary between the static-vrings host
and remote. The likely mapping is host transmit channel 0 / receive channel 1
and remote transmit channel 1 / receive channel 0. It remains provisional until
confirmed against the final binding examples and a clean hardware probe.

## Shared-memory layout

The provisional direct window is:

| Property | Value |
| --- | --- |
| Physical base | `0x91100000` |
| Size | `0x00020000` (128 KiB) |
| End, exclusive | `0x91120000` |
| Linux mapping | `reserved-memory`, `no-map` |
| Zephyr alignment | 64 bytes |
| Static-vrings payload buffer | 768 bytes |
| Planned descriptors | 64 in each direction |

The window begins immediately after the existing 1 MiB RPMsg MICFIL reservation
and lies below the DSP image at `0x92400000`. The current allocation algorithm
needs approximately 101,952 bytes for 64 descriptors in each direction, so 128
KiB provides margin for alignment and metadata.

The address is not final until all of the following pass:

- final M7 and DSP ELF program-header ranges do not overlap it;
- the final Linux DT has no overlapping reservation or ordinary System RAM
  allocation;
- both remotes observe the same pattern through the intended cache policy; and
- the address is recorded in one shared source of truth used by both builds and
  the Linux overlay.

## Transport instances and endpoints

There is exactly one physical static-vrings IPC instance on each remote. It
exposes two logical endpoints:

- `imx8mp.mpipe.ctrl.v1` for negotiation, flow control, status, and faults.
- `imx8mp.mpipe.audio.v1` for audio frames.

The endpoints share the physical link, vrings, mailbox channels, session, and
failure domain. Endpoint close is a coordinated session operation; application
components must not independently tear down the underlying transport.

The backend uses `K_NO_WAIT` for transmit-buffer acquisition. Neither capture
nor a mailbox callback may block waiting for transport space.

## Wire format

All multibyte wire fields are little-endian. Receivers reject unknown mandatory
flags, nonzero reserved fields, malformed lengths, unsupported formats, and CRC
failures before the payload is exposed to audio processing.

### Common header

Every message begins with a 32-byte header:

| Offset | Field | Encoding |
| ---: | --- | --- |
| 0 | magic | Four bytes: `MIPC` |
| 4 | major | `u8` |
| 5 | minor | `u8` |
| 6 | type | `u8` |
| 7 | header length | `u8`, value 32 in v1 |
| 8 | flags | `le32` |
| 12 | session ID | nonzero `le32` |
| 16 | sequence | `le32` |
| 20 | payload length | `le32` |
| 24 | capture timestamp | microseconds modulo 2^32, `le32` |
| 28 | CRC32C | header with this field zeroed, followed by payload |

A new paired-core start creates a new, nonzero session ID. Sequence numbers are
monotonic modulo 2^32 within a session and independently tracked per endpoint
and direction. Duplicate, stale-session, and gap events increment distinct
counters. A duplicate or stale message is discarded; a gap is reported and the
stream continues unless a control transaction has lost required state.

### Audio descriptor

Audio messages append a 16-byte descriptor before PCM bytes:

| Offset | Field | Encoding |
| ---: | --- | --- |
| 0 | sample rate | `le32` |
| 4 | samples per channel | `le16` |
| 6 | channels | `u8` |
| 7 | sample format | `u8` |
| 8 | layout | `u8` |
| 9 | frame duration ms | `u8` |
| 10 | reserved | `le16`, must be zero |
| 12 | PCM byte count | `le32` |

Version 1 audio is 16 kHz, signed 16-bit little-endian, stereo interleaved,
160 samples per channel, and 10 ms per frame. PCM occupies 640 bytes; header,
descriptor, and PCM total 688 bytes, which fits the 768-byte transport buffer.

### Control messages

Version 1 defines `HELLO`, `HELLO_ACK`, `CONFIG`, `CONFIG_ACK`, `START`,
`START_ACK`, `STOP`, `STOP_ACK`, `CREDIT`, `STATUS`, `ERROR`, and `HEARTBEAT`.
Requests and acknowledgements carry a transaction sequence. A major-version
mismatch prevents stream start. A minor-version mismatch is allowed only when
both sides agree that all requested capabilities are understood.

## Session state machine

Both peers implement the same externally observable states:

1. `BOOT`: local hardware and transport initialization.
2. `BIND`: both logical endpoints are bound.
3. `HELLO`: version, role, session, and capability agreement.
4. `CONFIGURED`: the fixed v1 audio format is accepted.
5. `PAUSED`: transport is ready but capture is not producing frames.
6. `STREAMING`: credits permit audio transfer.
7. `DRAINING`: M7 has stopped new capture submissions and outstanding receive
   ownership is being returned.
8. `FAULT`: no new audio is accepted; Linux must recover the pair.

Only acknowledged control transitions change streaming state. A control timeout
retries only an idempotent transaction with the same transaction identifier.
After the retry budget, the session faults. A message from an older session can
never advance the state machine.

## Flow control and buffer ownership

The HiFi4 receiver initially grants four audio credits after `START_ACK`; the
protocol maximum is eight. One credit authorizes exactly one audio message. A
credit is returned only after the HiFi4 has finished with the held receive
buffer and released it to the transport.

The M7 capture path validates metadata and confirms a credit before requesting
a transmit buffer. It acquires with `K_NO_WAIT`, copies once from the mpipe
buffer into the IPC-owned buffer, sends without another copy, then unreferences
the mpipe buffer. All header construction and validation that can fail occurs
before acquisition because the backend cannot roll a claimed transmit buffer
back.

If there is no credit or no transmit buffer, the M7 drops the newest captured
frame and increments the matching counter. It never overwrites an older queued
frame and never blocks capture. Any failure after successful transmit-buffer
acquisition is transport-fatal and triggers paired recovery.

On the HiFi4, the receive callback performs bounded validation, holds the
receive buffer, places only its pointer and metadata into a fixed-depth queue,
and returns. It performs no dynamic allocation and never blocks. The worker
processes or rejects the frame, releases the buffer exactly once, and returns a
credit. Queue-full behavior rejects the newest frame, releases it, and reports
pressure without leaking a credit.

## Audio pipeline

The M7 pipeline terminates the existing DMIC/mpipe capture path in the direct
IPC sink. The initial format on the wire remains the native 10 ms stereo frame.

The HiFi4 pipeline begins with an IPC source adapter. It converts each stereo
sample pair to mono using `(L + R) / 2` with a signed 32-bit intermediate, then
pairs two 10 ms frames into one 20 ms mono block. Those blocks feed the existing
one-second model window and then the micro-speech inference path.

The adapter resets partial 20 ms aggregation on session change, discontinuity,
format error, or stop. No samples from different sessions may share a model
window.

## Cache and ordering contract

The shared region is explicitly non-cacheable on both remotes for the initial
implementation unless the clean hardware probe demonstrates a supported,
matched coherent mapping. Device-tree and linker placement alone are not
accepted as proof of cache behavior.

Producer ordering is payload and descriptor writes, memory barrier, then MU3
notification. Consumer ordering is notification, memory barrier, then
descriptor and payload reads. The static-vrings backend's cache maintenance and
barriers must be audited against this contract on both architectures. If either
side uses a cacheable mapping, explicit clean/invalidate operations and cache
line ownership rules become mandatory before implementation approval.

## Clock, power, and access control

The direct design requires MU3 clock enable while either endpoint is active and
valid RDC access for M7 MU3_A PDAP18 and HiFi4 MU3_B PDAP31. SEMA42 ownership
must not deny either side's intended access. Linux remains responsible for
bringing the relevant AudioMix power domain and clocks into a usable state
before releasing the DSP.

Current offline clock state does not prove a defect. Before feature code, a
minimal provenance-clean probe must record:

- MU3 access from each core;
- one notification in each direction;
- the actual IRQ source and acknowledgement behavior on DSP IRQ 7;
- DDR pattern visibility in each direction; and
- cacheability/order behavior under repeated transfers.

The probe may be built and run only after its test procedure and exact firmware
hashes are reviewed. Existing installed `69cb3b08a12a` artifacts are prohibited.

## Fault handling and recovery

Protocol-fatal conditions include bad negotiated state, repeated control
timeout, send failure after buffer acquisition, ownership accounting failure,
or inability to make progress while heartbeats continue. Data validation
failures are counted and dropped; a configured threshold escalates persistent
corruption to protocol-fatal.

On fatal error:

1. Stop M7 capture submissions.
2. Mark both endpoints faulted and reject new application traffic.
3. Release all locally held receive buffers that can be safely accounted for.
4. Report final counters and reason to Linux-visible diagnostics.
5. Have Linux stop the M7 and then the HiFi4.
6. Restart HiFi4 and then M7 with a new session ID.

The first implementation does not attempt in-place vring repair or unilateral
remote recovery.

## Observability and console ownership

The M7 owns `ttyUSB3`. Linux owns `ttyUSB2`. The HiFi4 sends bounded diagnostic
events and counters over the control endpoint; it does not compete for a board
UART. Logging must be rate-limited and must never consume audio credits.

At minimum, both peers expose session ID, state, negotiated version, frames
sent/received, no-credit drops, no-buffer drops, queue-full drops, CRC/format
errors, duplicates, gaps, stale-session messages, outstanding credits, held
receive buffers, heartbeats, and the last fatal reason.

Every hardware test record includes kernel identity, active DTB SHA-256, both
firmware SHA-256 values, Zephyr and HAL commits, console mapping, start/stop
commands, and the final counters. Firmware uses unique filenames; generic or
stale installed binaries are never silently reused.

## Attribution and integration policy

Implementation starts from tested integration commit
`a730906a1f6d986a159966f5a488560e7ba4dd03` and preserves the tested NXP HAL
state. No `west update`, rebase onto moving `main`, or recreation of already
integrated mpipe work is part of this design.

Commit `69cb3b08a12a` and branch `gsoc/mp-ipc-plugin` are do-not-submit inputs.
They contain substantial work imported from Devanshi without preserved
attribution. Any reusable idea or change must be recovered from the original
authored commits associated with upstream work, especially PRs #114088 and
#114143, or independently reimplemented with review. Existing authorship must
remain visible in commit history; code must not be copied from the quarantined
branch.

## Verification stages

Implementation planning must keep these stages independently reviewable:

1. Freeze and record exact source, toolchain, DTS, and firmware inputs.
2. Run the approved minimal MU3/DDR probe and close the access, clock, IRQ,
   cache, address, and channel-mapping unknowns.
3. Add common protocol definitions with host-side serialization, validation,
   CRC, sequence, and state-machine tests.
4. Add the one physical static-vrings transport and two logical endpoints on
   both remotes, initially without audio.
5. Exercise control negotiation, credits, held-buffer accounting, saturation,
   and paired restart.
6. Connect the M7 mpipe sink and prove bounded newest-frame drops under load.
7. Connect the HiFi4 source, stereo-to-mono adapter, 20 ms aggregation, and
   model window; verify arithmetic and reset behavior with known vectors.
8. Add the production Linux overlay and orchestration, including suspend
   exclusion and exact start/stop order.
9. Run extended audio, inference, fault-injection, restart, and cold-boot tests
   while capturing the required provenance record.

## Remaining unknowns and approval gates

The following are intentionally unresolved and must be closed before their
dependent implementation stage:

- live RDC PDAP18/PDAP31 and SEMA42 permissions;
- who enables and retains the AudioMix MU3 clock/power domain in the production
  boot sequence;
- exact complementary MU channel indices and DSP IRQ 7 demultiplex behavior;
- verified M7 and HiFi4 cache attributes for the proposed DDR mapping;
- final shared-memory address after final ELF and Linux-memory validation;
- the clean probe's design, source provenance, binaries, and run procedure;
- the production orchestration interface and mechanism for suspend exclusion;
- acceptable timeout, retry, heartbeat, and persistent-corruption thresholds;
- recovery signaling from remote diagnostics to the Linux orchestrator; and
- whether the exact-6.6 MU2 one-shot is accepted for production or replaced by
  a reviewed BSP backport before release.

Approval gates are:

1. Review and approve this design specification.
2. Review and approve the implementation and hardware-probe plan.
3. Review the provenance-clean probe before any board execution.
4. Approve the final memory map and Linux device-tree change.
5. Review implementation and test evidence before installing production
   firmware or integrating branches.
6. Explicitly approve any push, upstream submission, or destructive board
   modification.
