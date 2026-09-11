# i.MX8MP M7-to-HiFi4 Direct IPC Design

Date: 2026-09-11

Status: Revised after independent review; approval required; implementation not authorized

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

Independent review of commit `761aee5a7b4f4d328223b9811f2211c5e2b8891c`
returned a no-go for planning. This revision addresses its lifecycle,
persistent-vring reset, buffer sizing, wire protocol, bootstrap, restart-test,
attribution, and approval-state findings. Because those changes are material,
the revised specification requires explicit approval before implementation
planning.

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
| Shared DDR placement | Address was open | `0x91100000` is free but write-through cached by the HiFi4 reset cache attributes; live Linux also reports unreserved System RAM at `0xa0000000` | Move the provisional window to a 256 KiB cache-bypass-aligned region at `0xa0000000`; final acceptance remains gated on a clean probe and final DT/ELF checks |
| Linux audio nodes | Not fully inventoried | RPMsg audio and RPMsg MICFIL nodes are enabled; physical MICFIL, SAI3, SDMA3, and sound nodes are disabled | The direct-MU3 production overlay must disable both Linux RPMsg audio nodes to avoid stale ownership and misleading devices |
| MU3 runtime state | Register permissions and clock state were unknown | With both remotes offline, `mu3_cg` is disabled and AudioMix is powered off; strict `/dev/mem` prevents read-only RDC inspection | MU3 clock/power and RDC permissions require a provenance-clean probe; no conclusion may be inferred from offline state |
| DSP clock ownership | MU3 clock owner was open | The active DSP node lists only `ocram`, `core`, and `debug`; the exact driver supports optional `per_clk1`...`per_clk18`, and the clock provider exposes MU3 root | Add MU3 root as `per_clk1`, making Linux DSP runtime PM the sole gate owner; verify on hardware |
| Pair-level clock hold | Not specified | The DSP parent device exposes runtime-PM `power/control=auto` and reports `runtime_status=suspended` while offline | The Linux pair supervisor must hold that device at `power/control=on` before either remote starts and until both are offline |
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
- Its firmware parser explicitly treats a missing ELF resource table as a
  warning and returns success. With no advertised vdev, remoteproc creates no
  Linux-to-DSP RPMsg transport and therefore has no normal virtqueue kick to
  send after boot.
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
                     MU3 notifications + 256 KiB shared DDR
                  M7 host  <==========================>  HiFi4 remote
                       one static-vrings physical instance
                         control endpoint + audio endpoint
```

Linux starts, stops, and recovers the pair. Linux does not inspect or forward
normal audio frames. The M7 is the static-vrings host and the HiFi4 is the
remote. MU3 is dedicated to this direct transport.

## Linux lifecycle and the MU2 bootstrap exception

Linux userspace runs a pair supervisor, provisionally named `mpipe-paird`. It
is the only component permitted to write either remoteproc `state` attribute.
It also opens an M7 management RPMsg character endpoint on the existing MU1
link, named `imx8mp.mpipe.mgmt.v1`. That endpoint carries pair commands,
state, counters, and faults; normal audio and direct-link credits never traverse
Linux.

The production start order remains HiFi4 first, then M7:

1. Take an exclusive pair lock and discover the remotes by their `name`
   attributes rather than assuming stable enumeration. Resolve the DSP parent
   device through its remoteproc `device` link, record its current
   `power/control`, write `on`, and require `power/runtime_status` to become
   `active` within 2 seconds. This Linux runtime-PM hold exists before either
   remote can access MU3.
2. Verify both remotes are offline, the expected firmware names and hashes are
   selected, and no conflicting audio service is active.
3. Start the HiFi4; require its state to become `running` within 5 seconds. On
   the exact 6.6 BSP this includes its MU2 ready handshake.
4. Start the M7; require its state to become `running` and the MU1 management
   endpoint to appear within 5 seconds.
5. Require an M7 `PAIR_STATE` report with state `PAUSED` within another 5
   seconds. It proves a new direct-link session is ready; it does not start
   capture.
6. Send an idempotent `PAIR_START` with a nonzero transaction ID. Require
   `PAIR_START_ACK` within 2 seconds before reporting the service active.

The alternative M7-first test uses the same steps except that steps 3 and 4
are reversed. It is valid only under the already-active pair-level runtime-PM
hold; ad-hoc manual starts are outside the design.

Any start failure rolls back everything started by that transaction: stop the
M7 if it reached running, then stop the HiFi4, verify both state attributes are
offline, restore the DSP parent's prior `power/control`, and mark the pair
faulted. A repeated start request returns the current state and never starts
either core twice.

For an orderly stop, the supervisor sends `PAIR_QUIESCE` over MU1. The M7 stops
new capture, completes the direct `STOP` transaction, accounts for all credits
and held buffers, then returns `PAIR_QUIESCED`. The supervisor allows 2 seconds,
then stops the M7 and the HiFi4 in that order, verifying each becomes offline
within 5 seconds. If quiesce or a normal stop times out, it force-writes `stop`
to the M7 first and then the HiFi4, records an unclean shutdown, and requires a
fresh paired session before capture may resume. The runtime-PM hold is restored
to its prior value only after both remotes are confirmed offline. If either
cannot be stopped, the supervisor retains the hold, reports manual intervention
required, and does not risk gating MU3 while M7 code may still execute.

The supervisor maintains a 500 ms management heartbeat with the M7 and faults
after three missed replies. A DSP direct-link error is relayed by the M7 as
`PAIR_FAULT`; an M7-local fatal error is reported directly. If the M7 crashes
or its endpoint disappears, Linux detects that without relying on M7 relay.
Every such fault invokes the same paired stop/restart policy. One-core restart
while its peer continues is prohibited.

The management protocol uses the common 40-byte header below with stream ID
zero. Its control prefix and transaction rules are identical to the direct
control endpoint. Management-only message types are `PAIR_QUERY` (`0x40`),
`PAIR_STATE` (`0x41`), `PAIR_START` (`0x42`), `PAIR_START_ACK` (`0x43`),
`PAIR_QUIESCE` (`0x44`), `PAIR_QUIESCED` (`0x45`), and `PAIR_FAULT` (`0x46`).
Their body is the 20-byte `STATUS` body defined below; command requests set
state to the requested target and zero all counters. `PAIR_FAULT` uses the
16-byte `ERROR` body. Linux refuses an ACK whose transaction ID does not match
the outstanding command.

On the exact 6.6 BSP, the HiFi4 firmware triggers MU2_B general interrupt zero
exactly once per boot, after minimal local initialization and before waiting on
MU3. Linux binds that to its `<&mu2 3 0>` receive-doorbell channel and clears
its ready flag again on stop. The firmware sends no data word, enables no MU2
receive or general-interrupt source, creates no Zephyr MU2 mailbox device, and
does not use MU2 for application traffic. Its resource table must not advertise
a Linux RPMsg vdev. Acceptance of an ELF with no resource table and of the
one-shot notification is an explicit clean-probe test.

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
- add `IMX8MP_CLK_AUDIOMIX_MU3_ROOT` to the DSP remoteproc `clocks` list as
  `per_clk1`, so the exact DSP driver enables it with the AudioMix power domain
  for the remote's runtime and disables it at runtime suspend;
- disable the existing RPMsg audio and RPMsg MICFIL nodes for this operating
  mode; and
- leave physical audio peripherals assigned according to the selected M7
  capture topology.

The M7 Zephyr device tree gains MU3_A at its verified address and IRQ. Its
existing MU1 management path remains separate. The HiFi4 Zephyr device tree
gains only the MU3_B mailbox interrupt device; it must not create a MU2 mailbox
interrupt device. The boot-only MU2 ready write is a narrowly isolated platform
operation.

NXP's paired static-vrings board examples establish the complementary channel
mapping: M7 host `tx = 0`, `rx = 1`; HiFi4 remote `rx = 0`, `tx = 1`. The clean
probe must still demonstrate both directions on this SoC before feature work.

## Shared-memory layout

The provisional direct window is:

| Property | Value |
| --- | --- |
| Physical base | `0xa0000000` |
| Size | `0x00040000` (256 KiB) |
| End, exclusive | `0xa0040000` |
| Linux mapping | `reserved-memory`, `no-map` |
| Zephyr alignment | 128 bytes |
| Static-vrings transport buffer | 1024 bytes |
| RPMsg application-visible maximum | 1008 bytes after its 16-byte header |
| Planned descriptors | 64 in each direction |

Both Zephyr static-vrings nodes receive only
`0xa0000000-0xa003ff7f` (262,016 bytes). The aligned retained-generation block
occupies `0xa003ff80-0xa003ffff`; it is never included in the backend's memory
region. Linux reserves the enclosing 256 KiB as one unit.

The live Linux map currently treats this range as unreserved System RAM. The
production `reserved-memory` node therefore removes it from the allocator. The
M7 MPU maps `0x80000000-0xbfffffff` as normal non-cacheable memory. The HiFi4
reset `CACHEATTR` maps 512 MiB windows and gives `0xa0000000-0xbfffffff` cache
bypass, whereas the earlier `0x91100000` candidate is write-through cached.

With 128-byte alignment, 1024-byte transport buffers, and 64 descriptors, each
vring is 1,920 bytes. The backend requires
`2 * (64 * 1024 + 1920) + 128 = 135,040` bytes, including its status block.
An additional 128-byte generation block makes the exact v1 requirement 135,168
bytes. The 256 KiB reservation provides margin but cannot support 128 descriptors:
that configuration requires 269,568 bytes. Descriptor count is therefore
fixed at 64 unless the reservation and sizing proof are reviewed together.

The address is not final until all of the following pass:

- final M7 and DSP ELF program-header ranges do not overlap it;
- the final Linux DT has no overlapping reservation or ordinary System RAM
  allocation;
- both remotes observe the same pattern through the intended cache policy;
- Linux excludes the entire window after the overlay is applied; and
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

The M7 host is the sole owner of persistent-vring reset and initialization.
`CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET` is enabled on the host: its
pre-kernel initialization clears the shared status block, and host OpenAMP
creation clears and initializes both vrings. The HiFi4 never clears a vring.

The last 128 bytes of the direct reservation are a separately aligned retained
generation block, not backend status. Its fixed little-endian layout is:

| Offset | Field | Rule |
| ---: | --- | --- |
| 0 | magic | four bytes `MGEN` |
| 4 | version / length | `le16` 1 / `le16` 128 |
| 8 | host generation / inverse | nonzero `le32` and bitwise inverse `le32` |
| 16 | DSP-accepted generation / inverse | `le32` and bitwise inverse `le32` |
| 24 | host state | `le32`: 0 initializing, 1 published |
| 28..127 | reserved | zero |

On every M7 boot, the host writes state 0, completes backend status/vring reset,
executes a release barrier, increments the retained host generation (invalid or
wrapped state becomes 1), writes its inverse, and publishes state 1. It opens
the host backend only after publication. The DSP waits up to 5 seconds for a
valid published generation different from its retained accepted generation,
then claims it before opening the remote backend. The DSP writes the candidate
generation and inverse into the accepted fields, executes a release barrier,
reads both back, and requires a valid match before any backend or MU3 access.
A reset during a torn claim is safe because no transport access has happened;
a reset after a valid claim sees an equal generation and cannot reopen against
the same host. Immediately before backend open, the DSP executes an acquire
barrier and rereads host state, generation, and inverse. State must still be
published, the generation pair must still be valid, and it must equal the
claimed candidate. Any mismatch faults without backend access. Because host
reset precedes generation publication, a nonzero backend status observed after
that revalidation belongs to the new host even if the DSP did not sample the
intervening zero.

This rule supports both boot orders. A DSP-first boot waits for a new host
generation; an M7-first boot finds the new generation already published. A
host-generation change after a DSP has claimed one is fatal rather than an
in-place rebind. A DSP reboot after any backend access finds its durable claim
equal to the old host generation and times out into paired recovery. HELLO and
its new nonzero session ID provide a second generation check before application
traffic.

## Wire format

All multibyte wire fields are little-endian. Receivers reject unknown mandatory
flags, nonzero reserved fields, malformed lengths, unsupported formats, and CRC
failures before the payload is exposed to audio processing.

### Common header

Every message begins with a 40-byte header:

| Offset | Field | Encoding |
| ---: | --- | --- |
| 0 | magic | Four bytes: `MIPC` |
| 4 | major | `u8` |
| 5 | minor | `u8` |
| 6 | type | `u8` |
| 7 | header length | `u8`, value 40 in v1 |
| 8 | flags | `le32` |
| 12 | session ID | nonzero `le32` |
| 16 | stream ID | `le32`; 0 control/management, 1 audio |
| 20 | sequence | `le32` |
| 24 | payload length | `le32` |
| 28 | timestamp | microseconds modulo 2^32, `le32` |
| 32 | format generation | `le32`; 0 control, initially 1 audio |
| 36 | CRC32C | header with this field zeroed, followed by payload |

Version 1.0 recognizes flags bit 0 `ACK_REQUIRED`, bit 1 `RETRY`, bit 2
`DISCONTINUITY`, and bit 3 `FATAL`. Bits 4 through 31 must be zero. CRC32C uses
the Castagnoli polynomial and covers exactly the 40-byte header with its CRC
field zeroed, then `payload length` bytes. Its complete parameters are width
32, normal polynomial `0x1edc6f41` (reflected form `0x82f63b78`), initial
remainder `0xffffffff`, reflected input and output, and final XOR
`0xffffffff`. The check value for the nine ASCII bytes `123456789` is
`0xe3069283`. The resulting CRC is serialized little-endian.

A new paired-core start creates a new, nonzero session ID. Sequence numbers are
monotonic modulo 2^32 starting at zero, independently tracked per stream and
direction. Duplicate, stale-session, and gap events increment distinct
counters. A duplicate or stale message is discarded; an audio gap sets a
discontinuity and resets partial aggregation. A control gap faults the session
if it prevents completion of the single outstanding transaction. The M7
creates the session ID after every host boot; the DSP accepts it only during a
fresh HELLO after the retained-generation bootstrap. Format generation starts at one
and increments on each accepted CONFIG change. Version 1 forbids live format
change while streaming.

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

Version 1 audio is 16 kHz, signed 16-bit little-endian (`format = 1`), stereo
interleaved (`layout = 1`), 160 samples per channel, and 10 ms per frame. PCM
occupies 640 bytes; header, descriptor, and PCM total 696 bytes, which fits the
1008-byte application-visible maximum.

### Control messages

Version 1.0 assigns the following message types:

| Value | Type | Body |
| ---: | --- | --- |
| `0x01` / `0x02` | `HELLO` / `HELLO_ACK` | HELLO, 16 bytes |
| `0x03` / `0x04` | `CONFIG` / `CONFIG_ACK` | audio descriptor, 16 bytes |
| `0x05` / `0x06` | `START` / `START_ACK` | START, 8 bytes |
| `0x07` / `0x08` | `STOP` / `STOP_ACK` | STOP, 8 bytes |
| `0x09` | `CREDIT` | CREDIT, 8 bytes |
| `0x0a` | `STATUS` | STATUS, 20 bytes |
| `0x0b` | `ERROR` | ERROR, 16 bytes |
| `0x0c` / `0x0d` | `HEARTBEAT` / `HEARTBEAT_ACK` | HEARTBEAT, 8 bytes |
| `0x20` | `AUDIO` | audio descriptor followed by PCM |

Every control payload begins with an 8-byte prefix: transaction ID `le32`,
result `le16`, and following body length `le16`. Requests use a nonzero
transaction ID and result zero; acknowledgements echo that ID. Unsolicited
`CREDIT`, `STATUS`, and `ERROR` use transaction ID zero. Result values are 0
success, 1 unsupported version, 2 invalid state, 3 invalid argument, 4 busy,
and 5 internal error. Other values are reserved. Only one acknowledged control
transaction may be outstanding per direction. Timeout is 1 second; retry once
with the same ID and `RETRY`, then fault.

The exact control bodies are:

- HELLO: role `u8` (1 M7 host, 2 DSP remote), endpoint count `u8` (2), credit
  maximum `le16` (1..8), capability bits `le32` (zero in v1), maximum message
  `le32` (1008), heartbeat milliseconds `le16` (500), and control timeout
  milliseconds `le16` (1000). Its ACK contains the selected values.
- CONFIG: the 16-byte audio descriptor above. Its ACK echoes the selected
  descriptor. The negotiated format generation is 1 for subsequent AUDIO
  messages; CONFIG itself retains control-stream generation zero.
- START: initial credits `le16` (requested 4), queue depth `le16` (requested
  4), and first audio sequence `le32` (0). Its ACK contains selected values.
- STOP: reason `le16`, mode `u8` (1 means drain), reserved `u8` (zero), and
  last submitted/released audio sequence `le32`. Its ACK reports the last
  released sequence and succeeds only when held-buffer and credit accounting
  is balanced.
- CREDIT: returned count `le16`, negotiated limit `le16`, and last released
  audio sequence `le32`. The sender rejects zero returns and totals above the
  negotiated limit.
- STATUS: state `u8`, last error `u8`, credits `le16`, held buffers `le16`,
  queue depth `le16`, last received sequence `le32`, no-credit drops `le32`,
  and queue-full drops `le32`.
- ERROR: code `le16`, subsystem `u8`, severity `u8`, offending type `u8`, three
  reserved zero bytes, offending sequence `le32`, and implementation-defined
  detail `le32`. Severity 2 is fatal; values 0 and 1 are informational and
  recoverable.
- HEARTBEAT: uptime milliseconds `le32`, state `u8`, and three reserved zero
  bytes. Its ACK echoes the request body.

Unknown types are rejected. An unknown control type receives `ERROR` when it is
safe to do so; an unknown audio type is dropped and counted. A major-version
mismatch prevents stream start. A higher minor version is accepted only if all
mandatory flags and capabilities are understood.

## Session state machine

Both peers implement the same externally observable states:

1. `BOOT` (0): local hardware and transport initialization.
2. `BIND` (1): both logical endpoints are bound.
3. `HELLO` (2): version, role, session, and capability agreement.
4. `CONFIGURED` (3): the fixed v1 audio format is accepted.
5. `PAUSED` (4): transport is ready but capture is not producing frames.
6. `STREAMING` (5): credits permit audio transfer.
7. `DRAINING` (6): M7 has stopped new capture submissions and outstanding receive
   ownership is being returned.
8. `FAULT` (7): no new audio is accepted; Linux must recover the pair.

Only acknowledged control transitions change streaming state. A control timeout
retries only an idempotent transaction with the same transaction identifier.
After the retry budget, the session faults. A message from an older session can
never advance the state machine.

## Flow control and buffer ownership

The HiFi4 receiver grants four initial audio credits in `START_ACK`; the
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

The initial shared region is normal non-cacheable in the M7 MPU and cache
bypass in the HiFi4 reset mapping. This avoids cache-line ownership sharing
between processors while retaining normal-memory ordering semantics. The
128-byte transport alignment matches the HiFi4 data-cache line even though the
selected region bypasses that cache. Device-tree and linker placement alone
are not accepted as proof; the clean probe must verify attributes and repeated
bidirectional visibility at `0xa0000000`.

Producer ordering is payload and descriptor writes, memory barrier, then MU3
notification. Consumer ordering is notification, memory barrier, then
descriptor and payload reads. The static-vrings backend's cache maintenance and
barriers must be audited against this contract on both architectures. If either
side uses a cacheable mapping, explicit clean/invalidate operations and cache
line ownership rules become mandatory and the design must return for approval.
Zephyr OpenAMP/libmetal cache-operation configuration and the final map are
recorded in the build manifest even when the bypass mapping makes maintenance a
no-op.

## Clock, power, and access control

The direct design requires MU3 clock enable while either endpoint is active and
valid RDC access for M7 MU3_A PDAP18 and HiFi4 MU3_B PDAP31. SEMA42 ownership
must not deny either side's intended access. Linux owns the production MU3
clock lifetime: the DSP remoteproc node supplies
`IMX8MP_CLK_AUDIOMIX_MU3_ROOT` as `per_clk1`. The exact 6.6 DSP driver bulk
enables named peripheral clocks during runtime resume, after enabling its power
domains, and disables them during runtime suspend. Neither remote firmware may
toggle the AudioMix MU3 gate directly.

For the whole paired lifecycle, `mpipe-paird` forbids runtime suspend of the
DSP parent by holding its standard `power/control` at `on`. This is a Linux
pair-level hold on the same driver and power domain, not firmware clock
ownership. It makes M7-first testing safe and keeps MU3 available if the DSP
crashes while the M7 is quiescing or reporting the fault. The hold is released
only after both remoteprocs are offline; failure to stop either core leaves it
active for manual recovery.

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
failure to observe a newly published host generation/`DRIVER_OK` sequence,
three missed 500 ms heartbeats, or inability to make progress while heartbeats
continue. Data
validation failures are counted and dropped. Eight CRC or structural errors in
any rolling 1-second interval escalate persistent corruption to fatal; a clean
second resets the interval count.

On fatal error:

1. Stop M7 capture submissions.
2. Mark both endpoints faulted and reject new application traffic.
3. Release all locally held receive buffers that can be safely accounted for.
4. Report final counters and reason over the MU1 management endpoint if the M7
   remains alive; otherwise Linux detects the failed remoteproc or endpoint.
5. Have `mpipe-paird` stop the M7 and then the HiFi4, using the forced-stop
   path if normal quiesce cannot complete.
6. Restart HiFi4 and then M7 with a new session ID only under the supervisor's
   configured restart policy.

The first implementation does not attempt in-place vring repair or unilateral
remote recovery. An accidental one-core restart must never resume streaming:
the peer detects a session, heartbeat, or transport-state discontinuity and
requests paired recovery.

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

PR #96657 is also an upstream input requiring explicit provenance handling.
Its current head `433157262647ffcd042fb8621c6160ac71d5e8af` contains three
commits authored by Thong Phan (`101fb10a`, `e8edea20`, and `43315726`) covering
the sample, transport, and documentation. Any model/preprocessing reuse must be
content-reviewed from that original head, preserve Thong Phan's authored
commits or attribution, retain the TensorFlow Authors copyright and Apache-2.0
notices present in model assets, and be coordinated with the author before an
upstream submission. The open, changes-requested PR is a reference, not a
license to copy through the quarantined branch.

## Verification stages

Implementation planning must keep these stages independently reviewable:

1. Freeze and record exact source, toolchain, DTS, and firmware inputs.
2. Run the approved minimal MU3/DDR probe and close the access, clock, IRQ,
   cache, address, channel-direction, and MU2-ready unknowns. Test both remote
   start orders under the pair-level runtime-PM hold; DSP-first is production
   and M7-first must converge safely. Verify MU3 remains clocked while handling
   an injected DSP crash and is gated only after both remotes stop. This stage
   includes a separately reviewed minimal lifecycle harness that implements the
   exclusive lock, runtime-PM hold, ordered starts/stops, and rollback needed by
   the probe; it is not permissible to start remotes manually and defer those
   safeguards to the stage-8 production supervisor.
3. Add common protocol definitions with host-side serialization, validation,
   CRC, sequence, and state-machine tests.
4. Add the one physical static-vrings transport and two logical endpoints on
   both remotes, initially without audio.
5. Exercise control negotiation, credits, held-buffer accounting, saturation,
   paired restart, retained-memory reboot, and accidental one-core restart in
   each direction. The one-core cases must fault rather than resume streaming.
6. Connect the M7 mpipe sink and prove bounded newest-frame drops under load.
7. Connect the HiFi4 source, stereo-to-mono adapter, 20 ms aggregation, and
   model window; verify arithmetic and reset behavior with known vectors.
8. Add the production Linux overlay and pair supervisor, including verified
   sysfs transitions, rollback/forced-stop behavior, management-heartbeat
   failure, suspend exclusion, and exact start/stop order.
9. Run extended audio, inference, fault-injection, restart, and cold-boot tests
   while capturing the required provenance record.

## Remaining unknowns and approval gates

The following are intentionally unresolved and must be closed before their
dependent implementation stage:

- live RDC PDAP18/PDAP31 and SEMA42 permissions;
- DSP IRQ 7 MU2/MU3 demultiplex behavior under the direct firmware;
- hardware confirmation of the source-verified channel 0/1 direction mapping;
- hardware confirmation of the source-derived M7 non-cacheable and HiFi4
  cache-bypass attributes at `0xa0000000`;
- final shared-memory address after final ELF and Linux-memory validation;
- the clean probe's design, source provenance, binaries, and run procedure;
- end-to-end confirmation that the exact-6.6 driver observes the one-shot
  MU2_B general interrupt zero from the direct DSP firmware;
- practical validation of Linux-owned MU3 gate lifetime through the DSP
  remoteproc `per_clk1` entry and pair-level runtime-PM hold; and
- the operating-system mechanism used by `mpipe-paird` to enforce the stated
  suspend prohibition.

Approval gates are:

1. Review and approve this design specification.
2. Review and approve the implementation and hardware-probe plan.
3. Review the provenance-clean probe before any board execution.
4. Approve the final memory map and Linux device-tree change.
5. Review implementation and test evidence before installing production
   firmware or integrating branches.
6. Explicitly approve any push, upstream submission, or destructive board
   modification.
