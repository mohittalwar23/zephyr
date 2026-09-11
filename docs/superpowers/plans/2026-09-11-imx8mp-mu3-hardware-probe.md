# i.MX8MP MU3 Hardware Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Do not delegate implementation unless the user explicitly authorizes it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the i.MX8MP MU3 access, clock, IRQ, channel, shared-DDR, cache, and MU2-ready unknowns with provenance-clean Linux-booted firmware.

**Architecture:** A diagnostic-only Zephyr pair uses the production MU3 MBOX driver and a fixed shared pattern area, while a host-tested Python lifecycle harness holds DSP runtime PM, installs uniquely named artifacts, starts/stops both remoteprocs, and captures M7 UART. The probe branch is not merged wholesale into production.

**Tech Stack:** Zephyr C, NXP MBOX/MU HAL, Linux remoteproc, Linux DT, Python 3 `unittest`, SSH, `dtc`, pyserial if already installed or POSIX serial reads otherwise.

**Spec:** `docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`

## Global Constraints

- Work only in `/home/mt/zephyrproject/worktrees/imx8mp-mu3-probe` on `codex/imx8mp-mu3-probe`.
- Use exact Zephyr/HAL commits and SDK from the parent plan; do not update dependencies.
- Probe memory is the approved provisional `0xa0000000-0xa003ffff` reservation; board execution still requires separate approval of final ELF/DT checks and commands.
- DSP sends MU2_B general interrupt zero once, enables no MU2 receive interrupt, and instantiates only MU3 as a Zephyr MBOX device.
- Acquire the DSP runtime-PM hold before starting either remote and retain it until both are offline.
- Do not use installed `mu3-*` or `xcore-*` binaries and do not inspect them for implementation guidance.
- Do not drive SAI, MICFIL, codec, or speaker during this probe.
- Preserve and verify the active DTB and installed firmware before any deployment; unique names and SHA-256 are mandatory.

---

### Task 1: Add host-tested lifecycle and deployment primitives

**Files:**
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/probe_runner.py`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/check_reserved_memory.py`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/test_probe_runner.py`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/test_check_reserved_memory.py`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/README.rst`

**Interfaces:**
- Consumes: `/sys/class/remoteproc/*/{name,state,firmware,device}` and DSP parent `power/{control,runtime_status}`.
- Produces: `RemoteProc`, `RuntimePmHold`, `ArtifactInstaller`, and `PairRunner`; later tasks call `PairRunner.run(order)`.

- [ ] **Step 1: Write failing host tests for discovery, PM hold, rollback, and hashes**

```python
class PairRunnerTest(unittest.TestCase):
    def test_dsp_first_holds_pm_and_stops_m7_first(self):
        io = FakeIO(remotes={"imx-dsp-rproc": "offline", "imx-rproc": "offline"})
        PairRunner(io).run("dsp-first")
        self.assertEqual(io.events[:3], ["pm:on", "start:imx-dsp-rproc", "start:imx-rproc"])
        self.assertLess(io.events.index("stop:imx-rproc"), io.events.index("stop:imx-dsp-rproc"))
        self.assertEqual(io.events[-1], "pm:auto")

    def test_failed_second_start_rolls_back_and_restores_pm(self):
        io = FakeIO(fail_on="start:imx-rproc")
        with self.assertRaises(ProbeError):
            PairRunner(io).run("dsp-first")
        self.assertEqual(io.events[-2:], ["stop:imx-dsp-rproc", "pm:auto"])

    def test_dsp_loss_keeps_pm_until_m7_reports_quiesced_and_stops(self):
        io = FakeIO(probe_events=["MU3_PROBE PEER_LOST QUIESCED"])
        PairRunner(io).inject_dsp_loss()
        self.assertLess(io.events.index("stop:imx-dsp-rproc"),
                        io.events.index("probe:m7-quiesced"))
        self.assertLess(io.events.index("stop:imx-rproc"),
                        io.events.index("pm:auto"))
```

- [ ] **Step 2: Run the tests and verify the intended failure**

```bash
python3 -m unittest discover -v \
  -s samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts \
  -p 'test_*.py'
```

Expected: FAIL because `probe_runner` and its classes do not exist.

- [ ] **Step 3: Implement the minimal interfaces**

```python
@dataclass(frozen=True)
class Artifact:
    role: str
    local_path: Path
    remote_path: PurePosixPath
    sha256: str
```

The exact public methods are `PairRunner.__init__(io, timeout_s=5.0)`,
`preflight(m7, dsp, dtb)`, `start(order)`, `stop()`, `run(order)`, and
`inject_dsp_loss()`. `order` accepts only `dsp-first` or `m7-first`; run and
fault injection return a `ProbeResult` carrying timestamps, states, hashes, and
captured probe counters.

Implementation rules: discover by `name`, take `flock` on
`/run/lock/mpipe-pair.lock` before every preflight mutation and for the complete
run, monitor the persistent lock connection, and bound every SSH/SCP process.
Preflight commits hashes and artifacts only after the complete transaction
succeeds; failure disarms the prior gate and restores the previous firmware
selection while the lock remains live. Lock loss after a possible selection
change must report the exact previous values and require manual recovery.
Open/flush the UART only after the run lock is acquired, and restore and close
it before releasing that lock. If the lock connection dies during a run, close
the UART and dead process context while retaining the PM hold for recovery
under newly acquired exclusivity. Save the previous DSP
`power/control`, write `on`, wait for `runtime_status=active`, verify every
state write, stop M7 before DSP, and restore PM only when both are offline.
`ArtifactInstaller` must compare the local, copied, and remotely recomputed
SHA-256 values and refuse a generic firmware filename. The fault-injection
method starts a healthy pair, stops the DSP while retaining the PM hold,
requires and parses the M7 quiesced counters, stops M7, and only then restores
PM.

- [ ] **Step 4: Run tests and static checks**

```bash
python3 -m unittest discover -v \
  -s samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts \
  -p 'test_*.py'
python3 -m py_compile \
  samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/probe_runner.py
```

Expected: all tests pass and compilation returns zero.

- [ ] **Step 5: Commit the harness**

```bash
git add samples/subsys/ipc/ipc_service/imx8mp_mu3_probe
git commit -s -m "samples: ipc: add guarded i.MX8MP MU3 probe harness"
```

### Task 2: Add diagnostic M7 and DSP firmware

**Files:**
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/CMakeLists.txt`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/Kconfig`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/prj.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/src/main.c`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/src/probe_shared.h`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/src/dsp_fw_ready.c`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/boards/imx8mp_evk_mimx8ml8_m7.overlay`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/boards/imx8mp_evk_mimx8ml8_m7.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/boards/imx8mp_evk_mimx8ml8_adsp.overlay`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/boards/imx8mp_evk_mimx8ml8_adsp.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/tests.yaml`

**Interfaces:**
- Consumes: Zephyr MBOX API; M7 MU3_A `0x30e80000/IRQ138`; DSP MU3_B `0x30e90000/IRQ7`; shared probe record at `0xa0000000`.
- Produces: `struct mu3_probe_record`, banners `MU3_PROBE role=M7` or
  `MU3_PROBE role=DSP` with the exact 40-character Git commit, and final
  `MU3_PROBE PASS`/`FAIL`.

- [ ] **Step 1: Define compile-time-checked shared records**

```c
#define MU3_PROBE_MAGIC 0x5033554dU
#define MU3_PROBE_ITERATIONS 4096U

struct mu3_probe_record {
	uint32_t magic;
	uint32_t iteration;
	uint32_t writer;
	uint32_t reserved;
	uint32_t pattern[28];
};

BUILD_ASSERT(sizeof(struct mu3_probe_record) == 128);
BUILD_ASSERT((0xa0000000U % 128U) == 0U);
```

- [ ] **Step 2: Add overlays with exactly one DSP interrupt device**

The M7 overlay defines `mu3_a` with `compatible = "nxp,mbox-imx-mu"`,
`reg = <0x30e80000 DT_SIZE_K(64)>`, `interrupts = <138 0>`,
`rx-channels = <2>`, and `#mbox-cells = <1>`. The DSP overlay defines only
`mu3_b` at `0x30e90000`, `interrupts = <7 0 0>`, and the same receive-channel
count. Both define the same 256 KiB fixed shared-memory node; neither creates a
Zephyr MU2 node.

- [ ] **Step 3: Implement one-shot DSP READY and bidirectional pattern checks**

```c
int imx8mp_dsp_signal_linux_ready(void)
{
	MU_Type *const mu2_b = (MU_Type *)0x30e70000U;
	return MU_TriggerInterrupts(mu2_b, kMU_GenInt0InterruptTrigger) == kStatus_Success
		? 0 : -EIO;
}
```

Call this once after local memory/log initialization and before waiting on
MU3. The role-specific main loop writes the iteration, role, and deterministic
`pattern[i] = iteration ^ (0x9e3779b9U * (i + 1U))`, executes a release barrier,
sends MBOX channel 0, validates the peer record after an acquire barrier, and
replies on channel 1. M7 prints the final counters; DSP has no UART console.
The PASS record contains exact iteration, send, receive, receive-IRQ, and zero
pattern-error counters. The peer-loss record additionally contains explicit
zero post-quiesce-send and post-quiesce-write counters.
Both roles run a bounded peer-progress timer. If notifications stop, the M7
disables new probe submissions, performs no further shared-memory write or MU3
send, and prints `MU3_PROBE PEER_LOST QUIESCED`; the DSP enters the same local
quiesced state without a console. This mechanism exists only to prove safe
clock retention during the injected DSP-loss case.

- [ ] **Step 4: Build from empty directories and inspect generated artifacts**

```bash
set -euo pipefail
if test -e build/probe-m7 || test -e build/probe-dsp; then
  printf '%s\n' 'probe build path already exists; inspect it and choose a new reviewed path' >&2
  exit 1
fi
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
  ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe build -p always \
  -d build/probe-m7 -b imx8mp_evk/mimx8ml8/m7 \
  samples/subsys/ipc/ipc_service/imx8mp_mu3_probe
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
  ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe build -p always \
  -d build/probe-dsp -b imx8mp_evk/mimx8ml8/adsp \
  samples/subsys/ipc/ipc_service/imx8mp_mu3_probe
```

Inspect `zephyr.dts`, `.config`, linker maps, `readelf -lW`, and SHA-256. Fail
if either ELF overlaps `0xa0000000-0xa003ffff`, DSP enables a MU2 interrupt
device, addresses/IRQs differ, or M7 and DSP memory nodes disagree.

- [ ] **Step 5: Commit diagnostic firmware separately**

```bash
git add samples/subsys/ipc/ipc_service/imx8mp_mu3_probe
git commit -s -m "samples: ipc: add i.MX8MP MU3 diagnostic firmware"
```

- [ ] **Step 6: Rebuild deployable ELFs from new empty directories**

Repeat Step 4 from empty `build/probe-m7-final` and
`build/probe-dsp-final` directories after the commit. Confirm both ELFs embed
the exact new 40-character commit, and repeat all DTS, Kconfig, program-header,
map, and SHA-256 checks. The DSP SoC build may warn that `rimage` is unavailable;
the audited Linux remoteproc path consumes `zephyr.elf`, so absence of an
optional signed `zephyr.ri` is not a probe failure. Missing or invalid ELF
output is a failure.

### Task 3: Build the exact temporary Linux DTB

**Files:**
- Modify in a new exact-BSP worktree: `arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dts`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/linux/imx8mp-evk-rpmsg-mu3-probe.patch`

**Interfaces:**
- Consumes: NXP Linux commit `90192c5d29cb650fd7f7dd9094af14eefb38837d`.
- Produces: a reviewable DT patch and `imx8mp-evk-rpmsg-mu3-probe.dtb` with MU3 reservation and clock ownership.

- [ ] **Step 1: Create an isolated exact Linux worktree**

```bash
set -euo pipefail
if test -e /home/mt/linux-imx; then
  git -C /home/mt/linux-imx status --short --branch
  printf '%s\n' 'existing Linux checkout requires review before reuse' >&2
  exit 1
fi
test ! -e /home/mt/linux-imx-worktrees/imx8mp-mu3-probe
git clone --filter=blob:none https://github.com/nxp-imx/linux-imx.git /home/mt/linux-imx
git -C /home/mt/linux-imx fetch origin 90192c5d29cb650fd7f7dd9094af14eefb38837d
git -C /home/mt/linux-imx worktree add \
  /home/mt/linux-imx-worktrees/imx8mp-mu3-probe \
  -b codex/imx8mp-mu3-probe 90192c5d29cb650fd7f7dd9094af14eefb38837d
```

If `/home/mt/linux-imx` exists at execution time, this block records its status
and stops. Review it, then replace only the clone command with the already-
approved repository path; never clean or repurpose an existing checkout.

- [ ] **Step 2: Add the reservation and clock using exact symbols**

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
```

Also set `&rpmsg_audio` and `&rpmsg_micfil` to `status = "disabled"`. Do not
append `m7_dsp_ipc` to either remoteproc `memory-region` list.

- [ ] **Step 3: Compile, decompile, and collision-check**

```bash
git -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe diff --check
make -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe ARCH=arm64 imx_v8_defconfig
make -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe ARCH=arm64 \
  freescale/imx8mp-evk-rpmsg.dtb
dtc -I dtb -O dts -o /tmp/imx8mp-evk-rpmsg-mu3-probe.dts \
  /home/mt/linux-imx-worktrees/imx8mp-mu3-probe/arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dtb
rg -n 'a0000000|per_clk1|rpmsg_audio|rpmsg_micfil' \
  /tmp/imx8mp-evk-rpmsg-mu3-probe.dts
python3 /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe/samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/scripts/check_reserved_memory.py \
  /tmp/imx8mp-evk-rpmsg-mu3-probe.dts
```

The tested script parses every `reserved-memory/reg` entry as a half-open
64-bit interval, requires the exact direct reservation, and fails on any
overlap. Save the DTB hash and patch; do not install it.

- [ ] **Step 4: Commit the Linux change and export the patch**

```bash
git -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe add \
  arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dts
git -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe commit -s \
  -m "arm64: dts: imx8mp-evk: add M7-DSP MU3 probe memory"
mkdir -p \
  /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe/samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/linux
git -C /home/mt/linux-imx-worktrees/imx8mp-mu3-probe format-patch -1 --stdout > \
  /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe/samples/subsys/ipc/ipc_service/imx8mp_mu3_probe/linux/imx8mp-evk-rpmsg-mu3-probe.patch
```

### Task 4: Review, approve, and execute the physical probe

**Files:**
- Create: `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-mu3-clean-probe.log`

**Interfaces:**
- Consumes: exact firmware/DTB hashes from Tasks 2–3 and `PairRunner` from Task 1.
- Produces: a pass/fail evidence record closing or preserving each hardware unknown.

- [ ] **Step 1: Present the board-run packet and wait for explicit approval**

The packet must contain diffs, build return codes, ELF segments, generated DTS
snippets, firmware/DTB hashes, remote backup paths, boot-selection command,
rollback command, both start orders, and the guarantee that no speaker path is
enabled. Stop until the user approves this specific run.

- [ ] **Step 2: Back up and deploy only unique artifacts**

```bash
sha256sum \
  /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe/build/probe-m7-final/zephyr/zephyr.elf \
  /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe/build/probe-dsp-final/zephyr/zephyr.elf \
  /home/mt/linux-imx-worktrees/imx8mp-mu3-probe/arch/arm64/boot/dts/freescale/imx8mp-evk-rpmsg.dtb
ssh root@192.168.7.2 'sha256sum /lib/firmware/rproc-imx-rproc-fw /lib/firmware/imx/dsp/hifi4.bin /boot/imx8mp-evk-rpmsg.dtb'
```

Copy to filenames containing role and the first 12 hash characters. Do not
overwrite defaults; choose the temporary DTB through a recoverable bootloader
selection and never run `saveenv`.

- [ ] **Step 3: Run the approved matrix**

Run DSP-first and M7-first as separate paired cycles under the PM hold, 4,096
bidirectional pattern iterations, repeated cache stress, clean stop, and an
injected DSP stop while M7 detects peer loss and quiesces. Require correct MU2
READY, MU3 interrupt counters, zero pattern errors, no post-quiesce MU3/shared-
memory writes, MU3 clock retention throughout fault handling, and clock disable
only after both remotes are offline. Retained generation and stale-session
behavior belong to the subsequent production transport plan, not this hardware
probe.

- [ ] **Step 4: Restore and verify**

Restore the original DTB selection and firmware attributes, power-cycle if the
temporary boot selection requires it, then verify original hashes, both remotes
offline, `power/control=auto`, and `mu3_cg` disabled. Retain logs and uniquely
named test files only if the approval packet allowed them; otherwise remove
only the explicitly listed test copies.

- [ ] **Step 5: Record the gate**

Classify every claim as source, build-only, or hardware. A red RDC, clock, IRQ,
cache, address, READY, or rollback result blocks the transport plan and triggers
systematic debugging; do not edit production code around an unexplained probe.
