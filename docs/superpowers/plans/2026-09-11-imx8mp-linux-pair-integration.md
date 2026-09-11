# i.MX8MP Linux Pair Supervisor and Production DT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Do not delegate implementation unless the user explicitly authorizes it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Linux the sole, recoverable lifecycle authority for the M7/HiFi4 pair, reserve and clock the direct link in the exact NXP BSP device tree, and complete production deployment and regression evidence.

**Architecture:** A dependency-free Python supervisor discovers remote processors and RPMsg character devices dynamically, owns an exclusive lock and DSP runtime-PM hold, performs transactional paired start/quiesce/stop/recovery, and exchanges the common management protocol over MU1. A systemd service wraps the supervisor with a blocking sleep inhibitor for the entire paired run. The Linux DT keeps existing remoteproc carveouts, separately reserves the direct MU3 DDR window, adds MU3 as the DSP `per_clk1`, and disables legacy RPMsg audio nodes.

**Tech Stack:** NXP Linux 6.6 at `90192c5d29cb650fd7f7dd9094af14eefb38837d`, DTS/dtc, Linux remoteproc/RPMsg char UAPI, Python 3 standard library, unittest, systemd/systemd-inhibit, SSH.

**Spec:** `docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`

## Entry gate and constraints

- Direct transport and audio/inference evidence must be green before production installation. Host-side supervisor and DT work may be completed earlier, but not deployed.
- Linux userspace is the only writer of either remoteproc `state`; firmware never boots or resets its peer.
- Discover remoteprocs by `name`, RPMsg devices by channel name, and parent devices by resolved sysfs links. Enumeration numbers are never configuration.
- Take `/run/lock/mpipe-pair.lock` before any check or mutation. Hold DSP parent `power/control=on` before either start and until both remotes are confirmed offline.
- Production start is DSP then M7. Stop is M7 then DSP. Alternate M7-first exists only as an explicit test under the same PM hold.
- System suspend is blocked by `/usr/bin/systemd-inhibit --what=sleep --mode=block` while the service owns the pair. If that exact mechanism is unavailable on the target image, deployment is blocked and the design returns for approval.
- Do not overwrite default firmware or DTB files. Use content-addressed filenames and a recoverable, unsaved boot selection until final acceptance.

---

### Task 1: Create an isolated supervisor repository and protocol tests

**Files (new repository `/home/mt/zephyrproject/mpipe-paird`):**
- Create: `pyproject.toml`
- Create: `README.md`
- Create: `LICENSE`
- Create: `src/mpipe_paird/__init__.py`
- Create: `src/mpipe_paird/errors.py`
- Create: `src/mpipe_paird/protocol.py`
- Create: `tests/test_protocol.py`
- Create: `tests/vectors/mpipe-ipc-v1.json`
- Create: `.gitignore`

**Interfaces:**

```python
@dataclass(frozen=True)
class Message:
    type: int
    flags: int
    session_id: int
    stream_id: int
    sequence: int
    timestamp_us: int
    format_generation: int
    payload: bytes
```

The exact module functions are `encode(message) -> bytes`, `decode(wire) ->
Message`, and `make_management_request(message_type, transaction_id,
target_state) -> Message`.

- [ ] **Step 1: Initialize without touching an existing path**

```bash
set -euo pipefail
if test -e /home/mt/zephyrproject/mpipe-paird; then
  printf '%s\n' 'mpipe-paird path already exists; inspect before reuse' >&2
  exit 1
fi
mkdir /home/mt/zephyrproject/mpipe-paird
git -C /home/mt/zephyrproject/mpipe-paird init -b codex/main
```

If the path exists, inspect and report it; do not initialize, clean, or reuse it
without approval.

- [ ] **Step 2: Write failing cross-language golden-vector tests**

Copy the reviewed JSON vectors from
`tests/subsys/mpipe/ipc_protocol/vectors/mpipe-ipc-v1.json`, including
all management types, CRC32C, malformed headers, wrong ACK IDs, and payload
limits. Before committing, require `cmp` between the Zephyr source and
`tests/vectors/mpipe-ipc-v1.json`. Python tests must compare every encoded byte
to the C golden vector and reject the same invalid messages.

```python
class ProtocolTest(unittest.TestCase):
    def test_vectors_match_zephyr_codec(self):
        for vector in load_vectors():
            self.assertEqual(encode(message_from(vector)), bytes.fromhex(vector["wire"]))

    def test_ack_transaction_must_match(self):
        with self.assertRaises(ProtocolError):
            require_ack(outstanding=7, received=8)
```

- [ ] **Step 3: Run red, implement the minimal codec, then run green**

```bash
cd /home/mt/zephyrproject/mpipe-paird
cmp /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/tests/subsys/mpipe/ipc_protocol/vectors/mpipe-ipc-v1.json \
  tests/vectors/mpipe-ipc-v1.json
python3 -m unittest discover -v -s tests -p 'test_*.py'
python3 -m compileall -q src tests
```

Use only `struct`, `binascii` or a small reviewed CRC32C implementation, and
bounds-checked slices. The implementation must reproduce Castagnoli CRC
`0xe3069283` for `123456789`; `binascii.crc32` alone is not CRC32C.

- [ ] **Step 4: Commit the independent protocol layer**

```bash
git add pyproject.toml README.md LICENSE src tests .gitignore
git commit -s -m "protocol: add M7-HiFi4 management wire codec"
```

### Task 2: Implement tested remoteproc discovery, PM hold, and rollback

**Files:**
- Create: `src/mpipe_paird/io.py`
- Create: `src/mpipe_paird/remoteproc.py`
- Create: `src/mpipe_paird/runtime_pm.py`
- Create: `src/mpipe_paird/supervisor.py`
- Create: `tests/fakes.py`
- Create: `tests/test_remoteproc.py`
- Create: `tests/test_supervisor.py`

**Interfaces:**

`RemoteProcInventory.discover(sysfs=Path("/sys"))` returns an inventory and
`require_unique(name)` returns one `RemoteProc` or raises. `RuntimePmHold`
provides `acquire(timeout_s=2.0)` and `release_after_offline(remotes)`.
`PairSupervisor` provides `start(order="dsp-first")`, `quiesce_and_stop()`,
`force_stop(reason)`, and `recover(reason)`, each returning the resulting
`PairState` or raising a typed error.

- [ ] **Step 1: Write failing fake-sysfs lifecycle tests**

Cover duplicate/missing names, symlink-resolved DSP parent, firmware/hash
preflight, both-offline prerequisite, lock contention, previous PM value
restoration, two-second PM activation timeout, five-second starts/stops,
DSP-first default, M7-first test order, failure of first/second start, quiesce
timeout, forced M7-then-DSP stop, repeated idempotent start, failed stop retaining
PM `on`, and signal/exception cleanup.

```python
def test_second_start_failure_rolls_back_m7_then_dsp_and_restores_pm(self):
    io = FakeIO(fail={"start:imx-rproc"})
    with self.assertRaises(StartError):
        PairSupervisor(io).start()
    self.assertEqual(io.tail(3), ["stop:imx-dsp-rproc", "offline:both", "pm:auto"])
```

When only DSP was started, rollback stops DSP directly; when both may be live,
all stop paths order M7 before DSP. Tests must assert no PM restoration when
either state is not `offline`.

- [ ] **Step 2: Implement minimal verified sysfs operations**

Every write is followed by bounded polling of the corresponding state. Use
`os.open` plus
`fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)` for the pair lock, monotonic
deadlines, injectable clock/sleep/I/O, and structured event records. Never shell
concatenate a path or trust a configured enumeration number.

- [ ] **Step 3: Run lifecycle tests and fault permutations**

```bash
python3 -m unittest discover -v -s tests -p 'test_*.py'
python3 -m compileall -q src tests
```

Use subtests to inject failure at every mutation boundary and assert either the
original all-offline/PM state is restored or the PM hold is retained with a
manual-intervention error.

- [ ] **Step 4: Commit the lifecycle core**

```bash
git diff --check
git add src tests
git commit -s -m "supervisor: add transactional remoteproc pair lifecycle"
```

### Task 3: Implement RPMsg management and pair health policy

**Files:**
- Create: `src/mpipe_paird/rpmsg.py`
- Create: `src/mpipe_paird/health.py`
- Modify: `src/mpipe_paird/supervisor.py`
- Create: `tests/test_rpmsg.py`
- Create: `tests/test_health.py`
- Modify: `tests/test_supervisor.py`

**Interfaces:**

`RpmsgManagement` is a context manager with `wait_for_channel(name,
timeout_s=5.0)`, `request(message, timeout_s=2.0)`, and `close()`.
`PairHealth` provides `observe_state(message)`, `heartbeat_due(now)`, and
`fault_reason(now) -> str | None`.

- [ ] **Step 1: Audit the exact kernel UAPI before writing tests**

In the exact Linux worktree inspect `include/uapi/linux/rpmsg.h`,
`drivers/rpmsg/rpmsg_char.c`, `drivers/rpmsg/imx_rpmsg.c`, and the live target's
`/sys/class/rpmsg` plus `/dev/rpmsg*` layout while both remotes remain offline.
Record the exact `rpmsg_endpoint_info` layout, ioctl numbers, driver binding,
and discovery path in `README.md`. If the target lacks `rpmsg_char` support,
stop and report the BSP/config blocker rather than inventing another transport.

- [ ] **Step 2: Write failing RPMsg/UAPI and health tests**

Use fake file descriptors/ioctl/read/write/poll. Cover channel appearance,
endpoint creation/destruction, partial/oversize datagrams, exact transaction
matching, one retry with the same ID and RETRY flag, PAUSED within five seconds,
START_ACK/QUIESCED within two seconds, 500 ms heartbeat, three missed replies,
PAIR_FAULT, endpoint disappearance, stale session, unexpected message, and
clean close during rollback.

- [ ] **Step 3: Implement the exact UAPI and integrate start/stop**

After M7 reaches running, create/open only `imx8mp.mpipe.mgmt.v1`, require a
fresh session PAIR_STATE=PAUSED, then send idempotent PAIR_START. Normal stop
sends PAIR_QUIESCE and waits for PAIR_QUIESCED before sysfs stops. Health faults
call the common paired recovery policy; no management request forwards audio or
direct-link credits.

- [ ] **Step 4: Run tests under race/failure injection and commit**

```bash
python3 -m unittest discover -v -s tests -p 'test_*.py'
python3 -m compileall -q src tests
git diff --check
git add src tests README.md
git commit -s -m "supervisor: add RPMsg pair management and health policy"
```

### Task 4: Add CLI, configuration, systemd sleep inhibitor, and packaging

**Files:**
- Create: `src/mpipe_paird/config.py`
- Create: `src/mpipe_paird/cli.py`
- Create: `tests/test_config.py`
- Create: `tests/test_cli.py`
- Create: `packaging/mpipe-paird.service`
- Create: `packaging/mpipe-paird.json`
- Create: `packaging/install.sh`
- Modify: `pyproject.toml`
- Modify: `README.md`

**Interfaces:**

```text
mpipe-paird preflight --config /etc/mpipe-paird/config.json
mpipe-paird serve --config /etc/mpipe-paird/config.json
mpipe-paird status --config /etc/mpipe-paird/config.json
mpipe-paird stop --config /etc/mpipe-paird/config.json
```

- [ ] **Step 1: Write failing config/CLI/install tests**

Reject relative paths, generic firmware names, missing SHA-256, duplicate remote
names, invalid retry policy, unsupported start order, and nonexistent inhibitor.
Test SIGTERM quiesce, nonzero exit on retained PM hold/manual intervention,
structured status output, and installer refusal to overwrite a differing file.

- [ ] **Step 2: Implement a strict content-addressed configuration**

Configuration contains remote names, expected firmware filenames/hashes,
management channel, exact timeouts, heartbeat, restart policy, and evidence-log
path. It does not contain remoteproc numbers. Preflight recomputes installed
hashes and verifies both remotes offline before the service starts.

- [ ] **Step 3: Implement the service with a blocking sleep inhibitor**

Use this exact shape, adjusted only for the reviewed installation prefix:

```ini
[Unit]
Description=i.MX8MP M7/HiFi4 paired IPC service
After=systemd-udev-settle.service
Conflicts=sleep.target
Before=sleep.target

[Service]
Type=simple
ExecStart=/usr/bin/systemd-inhibit --what=sleep --mode=block --who=mpipe-paird --why="M7/HiFi4 pair active" /usr/bin/mpipe-paird serve --config /etc/mpipe-paird/config.json
Restart=on-failure
RestartSec=2
KillSignal=SIGINT
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
```

Before deployment, require `/usr/bin/systemd-inhibit` and verify with
`systemd-inhibit --list` that the lock is present while the service owns the
pair and absent after both remotes are offline. The daemon's SIGINT handler
performs quiesce and ordered stop before exiting; no second `ExecStop` process
contends for the pair lock. `Conflicts/Before` provides the system-sleep
orchestrator ordering, while the inhibitor prevents the sleep transaction from
passing the active service before cleanup completes.

- [ ] **Step 4: Run package tests and commit**

```bash
python3 -m unittest discover -v -s tests -p 'test_*.py'
python3 -m compileall -q src tests
git diff --check
git add pyproject.toml src tests packaging README.md
git commit -s -m "packaging: add guarded mpipe-paird systemd service"
```

### Task 5: Implement and prove the production Linux device tree

**Files (Linux worktree `/home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc`):**
- Modify: `arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dts`
- Create in supervisor repo: `packaging/linux/imx8mp-evk-rpmsg-m7-hifi4.patch`
- Create in supervisor repo: `tests/test_reserved_memory.py`

**Interfaces:** The root reservation is exactly `0xa0000000-0xa003ffff`,
`no-map`. The DSP remoteproc clock list adds
`IMX8MP_CLK_AUDIOMIX_MU3_ROOT` named `per_clk1`. Existing remoteproc management
carveouts stay unchanged; the direct reservation is not in either
`memory-region` list. `rpmsg_audio` and `rpmsg_micfil` are disabled.

- [ ] **Step 1: Create a separate exact-BSP production worktree**

```bash
set -euo pipefail
test -d /home/mt/linux-imx/.git
test ! -e /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc
git -C /home/mt/linux-imx fetch origin 90192c5d29cb650fd7f7dd9094af14eefb38837d
git -C /home/mt/linux-imx worktree add \
  /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc \
  -b codex/imx8mp-m7-hifi4-ipc 90192c5d29cb650fd7f7dd9094af14eefb38837d
```

Stop if either path or branch already exists with an unexpected identity.

- [ ] **Step 2: Write the reservation collision test first**

The Python test parses decompiled reserved-memory `reg` cells into half-open
64-bit intervals, asserts pairwise non-overlap, asserts exact containment of the
direct range, and rejects any remoteproc `memory-region` reference to it. First
run it against an intentionally overlapping fixture and the unmodified DTS to
show the expected failures.

- [ ] **Step 3: Apply the exact production DT changes**

```dts
m7_dsp_ipc: ipc@a0000000 {
	reg = <0 0xa0000000 0 0x00040000>;
	no-map;
};

&dsp {
	clocks = <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_OCRAMA_IPG>,
		 <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_DSP_ROOT>,
		 <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_DSPDBG_ROOT>,
		 <&audio_blk_ctrl IMX8MP_CLK_AUDIOMIX_MU3_ROOT>;
	clock-names = "ocram", "core", "debug", "per_clk1";
};

&rpmsg_audio { status = "disabled"; };
&rpmsg_micfil { status = "disabled"; };
```

Retain both remoteproc nodes and all prior management carveouts verbatim. The
clock symbols are from the exact BSP binding and must not be replaced by raw
indices.

- [ ] **Step 4: Build, decompile, compare, and validate**

```bash
set -euo pipefail
make -C /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc \
  ARCH=arm64 imx_v8_defconfig
make -C /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc \
  ARCH=arm64 freescale/imx8mp-evk-rpmsg.dtb
production_dts=$(mktemp /tmp/imx8mp-production.XXXXXX.dts)
dtc -I dtb -O dts -o "$production_dts" \
  /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc/arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dtb
python3 /home/mt/zephyrproject/mpipe-paird/tests/test_reserved_memory.py \
  "$production_dts"
```

Compare decompiled remoteproc nodes to the active DTB and source baseline.
Require exact reservation/no-map, disabled legacy audio nodes, unchanged
management carveouts/mailboxes, and `per_clk1`. Compare final M7/DSP ELF
program-header intervals and fail on overlap. Record DTB/ELF SHA-256.

- [ ] **Step 5: Commit and export the production patch**

```bash
set -euo pipefail
git -C /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc add \
  arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dts
git -C /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc commit -s \
  -m "arm64: dts: imx8mp-evk: reserve direct M7-DSP IPC"
mkdir -p /home/mt/zephyrproject/mpipe-paird/packaging/linux
git -C /home/mt/linux-imx-worktrees/imx8mp-m7-hifi4-ipc \
  format-patch -1 --stdout > \
  /home/mt/zephyrproject/mpipe-paird/packaging/linux/imx8mp-evk-rpmsg-m7-hifi4.patch
git -C /home/mt/zephyrproject/mpipe-paird add \
  packaging/linux tests/test_reserved_memory.py
git -C /home/mt/zephyrproject/mpipe-paird commit -s \
  -m "packaging: add i.MX8MP production DT patch"
```

### Task 6: Review deployment packet and install only approved artifacts

**Files:**
- Create: `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-m7-hifi4-production-deploy.log`
- Modify: `/home/mt/zephyrproject/review/PROGRESS.md`

- [ ] **Step 1: Run the complete host-side verification fresh**

```bash
cd /home/mt/zephyrproject/mpipe-paird
python3 -m unittest discover -v -s tests -p 'test_*.py'
python3 -m compileall -q src tests
git diff --check
git log --format='%H%x09%an%x09%ae%x09%s%n%b'
```

Run all Zephyr protocol/session/transport/element/reference tests and fresh M7
and DSP builds from the preceding plans. Run `git diff --check`, DCO, license,
attribution, and generated-artifact checks in every repository.

- [ ] **Step 2: Present the final-memory/deployment packet and wait**

Include exact Linux/Zephyr/HAL/supervisor commits, all diffs, test return codes,
DT/ELF interval proof, DTB/firmware/package hashes, current board hashes, unique
destination names, boot selection, rollback, service commands, expected RPMsg
devices, and the no-speaker DTS proof. This is both approval gate 4 (final map/
DT) and gate 5 (production installation). Do not deploy until explicitly
approved.

- [ ] **Step 3: Back up state and stage content-addressed files**

Record active DTB and existing firmware hashes plus remoteproc firmware
attributes. Copy only new names containing the first 12 SHA-256 characters.
Run the installer in dry-run mode, then install only paths listed in the packet.
Select the new DTB through a recoverable bootloader environment change without
`saveenv`; do not overwrite the known-good DTB or firmware.

- [ ] **Step 4: Boot, re-audit, and enable the pair service**

After the approved reboot, verify kernel identity, active DTB hash,
`/proc/iomem` exclusion of the 256 KiB reservation, both remoteprocs offline,
legacy RPMsg audio absent, remoteproc/RPMsg char modules available,
`systemd-inhibit` available, and MU3 initially gated. Run `mpipe-paird preflight`
before enabling/starting the service.

- [ ] **Step 5: Execute the production acceptance matrix**

Run: normal DSP-first start/stop; approved M7-first test; duplicate start;
normal quiesce; forced stop; failure at every start boundary; wrong firmware
hash; missing management endpoint; wrong ACK; three missed heartbeats; M7 crash;
DSP crash while M7 quiesces; both one-core restart cases; paired recovery;
credit/no-buffer/queue pressure; transported fixtures; live speech; 60-minute
soak; and cold boot. Verify the sleep inhibitor appears while active and is
removed after the pair is offline. Attempting suspend while active must be
blocked; an approved suspend after clean service stop may proceed.

Require MU3 clock active for the entire pair lifetime—including DSP crash
handling—and disabled only after both remotes are offline and PM control is
restored. Require new retained generation/session after paired restart, no stale
stream resume, exact buffer/credit balance, bounded audio drops, and matching
fixture results.

- [ ] **Step 6: Roll back or retain only after evidence review**

On any failure, stop M7 then DSP, retain PM if either is not offline, select the
known-good DTB for the next boot, disable the new service, and report exact
state. On success, leave the approved candidate running only if the packet
authorized it. Do not delete unique test files, modify defaults, push, merge, or
save bootloader state without separate approval.

### Task 7: Produce the final review packet

**Files:**
- Modify: `/home/mt/zephyrproject/review/PROGRESS.md`
- Create: `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-m7-hifi4-final.log`

- [ ] **Step 1: Record evidence with explicit classifications**

For every claim record source-inspected, build-only, host-tested, or hardware-
verified; command, return code, timestamp, hashes, start order, generation,
session ID, counters, timings, and final board state. Preserve raw logs beside
the summary.

- [ ] **Step 2: Re-audit history, attribution, and hygiene**

Show that tested branches are unchanged, diagnostic work stayed isolated,
Thong Phan owns the imported inference commit, TensorFlow headers remain,
quarantined commit `69cb3b08a12a` is not an ancestor/source, all new commits
have DCO sign-off, and no binaries/logs/DTBs/model outputs are tracked.

- [ ] **Step 3: Present exact integration choices and stop**

Report all branch heads and diffs, remaining limitations (paired restart only,
no active-pair suspend, v1 fixed format), rollback state, and review findings.
Ask separately before any merge, push, upstream submission, default-file
replacement, bootloader persistence, or destructive cleanup.
