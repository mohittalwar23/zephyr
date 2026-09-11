# i.MX8MP M7-to-HiFi4 IPC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Do not delegate implementation unless the user explicitly authorizes it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver Linux-managed M7 and HiFi4 firmware that transports M7 microphone audio directly over MU3/static-vrings into a HiFi4 micro-speech pipeline.

**Architecture:** Linux owns both remoteproc lifecycles and an M7 MU1 management endpoint, while one M7-host/HiFi4-remote static-vrings instance uses MU3 and reserved DDR for control and audio. Work is split into four plans with hard gates: clean hardware proof, protocol/transport, audio/inference, then Linux production integration and regression.

**Tech Stack:** Zephyr C/C++, IPC Service static-vrings, OpenAMP/libmetal, NXP i.MX8MP MBOX/MU, mpipe, TensorFlow Lite Micro, Linux remoteproc/RPMsg, Python 3, Linux device tree.

**Spec:** `docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`

## Global Constraints

- Zephyr implementation base is exactly `a730906a1f6d986a159966f5a488560e7ba4dd03` plus the approved design/plan commits; NXP HAL remains `1123e43d6350489036f5a14d296c2c18cbd39478`.
- Use `/home/mt/zephyrproject/.venv/bin/west` with `-z` set to the active
  execution worktree and Zephyr SDK `/home/mt/zephyr-sdk-1.0.1`; otherwise west
  silently selects the main checkout. Do not run `west update`.
- Preserve `gsoc/evk-imx8mp-integration-final`, all tested worktrees, SOF `dai-burst-fix`, and every existing untracked file.
- Never copy from `gsoc/mp-ipc-plugin` or commit `69cb3b08a12a`; recover attributable work from its original commits or implement independently.
- Preserve Thong Phan as author for reused PR #96657 model/preprocessing content and retain TensorFlow Authors/Apache-2.0 notices.
- Production transport is one static-vrings physical instance, M7 host `tx=0/rx=1`, HiFi4 remote `rx=0/tx=1`, with 128-byte alignment and 1024-byte transport buffers.
- Linux reserves `0xa0000000-0xa003ffff`; the backend receives 262,016 bytes and the generation block occupies `0xa003ff80-0xa003ffff`.
- Linux holds `/sys/class/remoteproc/<dsp>/device/power/control=on` before either remote starts and until both are confirmed offline.
- DSP sends one raw MU2_B general interrupt zero per boot and creates no Zephyr MU2 interrupt device or Linux RPMsg vdev.
- No real-time callback blocks, allocates dynamically, performs inference, or waits for a transmit buffer; all transmit acquisition is `K_NO_WAIT`.
- Every behavior change follows red-green-refactor TDD and every reviewable layer is committed separately with `Signed-off-by: Mohit Talwar <talwarmohit2005@gmail.com>`.
- No board execution, firmware/DTB installation, push, PR, force operation, or destructive modification occurs without its explicit gate approval.
- Hardware logs go under `/home/mt/zephyrproject/review/hardware-logs/`; generated binaries, logs, scratch DTBs, and model test output remain untracked.

---

## Plan set and dependency order

1. `docs/superpowers/plans/2026-09-11-imx8mp-mu3-hardware-probe.md`
   closes RDC, clock, IRQ, channel, cache, DDR, and MU2-ready uncertainties.
2. `docs/superpowers/plans/2026-09-11-imx8mp-mu3-transport.md`
   implements the wire codec, generation/session logic, static-vrings transport,
   and deterministic two-core baseline.
3. `docs/superpowers/plans/2026-09-11-imx8mp-m7-hifi4-audio-inference.md`
   implements mpipe elements, synthetic and DMIC graphs, attributed model import,
   reference inference, and separately gated optimization.
4. `docs/superpowers/plans/2026-09-11-imx8mp-linux-pair-integration.md`
   implements the production Linux DT, pair supervisor, deployment workflow,
   and final regression/evidence matrix.

Do not start a dependent plan while its predecessor's gate is red or ambiguous.
The hardware probe branch is diagnostic and must not be merged wholesale into
the production branch.

### Task 1: Freeze execution branches and evidence

**Files:**
- Modify: `/home/mt/zephyrproject/review/PROGRESS.md`
- Create: `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-m7-hifi4-execution-baseline.log`

**Interfaces:**
- Consumes: approved spec commit `f63e96a308f657451914532f52092664d610faf1` and this plan-set commit.
- Produces: immutable source IDs and branch/worktree paths consumed by all four plans.

- [ ] **Step 1: Re-run the read-only identity audit**

```bash
git -C /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc-design status --short --branch
git -C /home/mt/zephyrproject/worktrees/evk-imx8mp status --short --branch
git -C /home/mt/zephyrproject/modules/hal/nxp status --short --branch
git -C /home/mt/sofws/sof status --short --branch
ssh root@192.168.7.2 'uname -a; for r in /sys/class/remoteproc/remoteproc*; do cat "$r/name" "$r/state" "$r/firmware"; done'
```

Expected: tested repositories retain their recorded heads; both remotes are
offline. Stop if an identity differs and record the discrepancy before doing
anything else.

- [ ] **Step 2: Create production and diagnostic worktrees without changing tested branches**

```bash
set -euo pipefail
ipc_plan_commit=$(git -C /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc-design rev-parse HEAD)
test ! -e /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc
test ! -e /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe
git -C /home/mt/zephyrproject/zephyr worktree add \
  /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
  -b codex/imx8mp-m7-hifi4-ipc "$ipc_plan_commit"
git -C /home/mt/zephyrproject/zephyr worktree add \
  /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe \
  -b codex/imx8mp-mu3-probe "$ipc_plan_commit"
```

Record the printed `ipc_plan_commit` value in the baseline log before creating
either worktree.

- [ ] **Step 3: Record the evidence baseline**

Use `apply_patch` to add the exact command output, local date, handoff/spec/plan
hashes, active DTB hash, kernel commit, console ownership, and worktree paths to
the execution-baseline log. Label it source inspection; do not claim a build or
hardware result.

- [ ] **Step 4: Verify both execution worktrees are pristine**

```bash
git -C /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc diff --check
git -C /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc status --short
git -C /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe diff --check
git -C /home/mt/zephyrproject/worktrees/imx8mp-mu3-probe status --short
```

Expected: both outputs are empty. The external PROGRESS and hardware-log files
are deliberately not added to a Zephyr commit.

### Task 2: Execute the four plans at their gates

**Files:**
- Modify: `/home/mt/zephyrproject/review/PROGRESS.md`

**Interfaces:**
- Consumes: each sub-plan's final evidence record.
- Produces: one gate decision before the next plan begins.

- [ ] **Step 1: Complete the hardware-probe plan and stop at its board gate**

Run every non-board step in the hardware-probe plan. Present source diffs,
build results, local/deployed hashes, DTB rollback method, and exact board
commands for approval. Do not interpret this plan approval as board-run
approval.

- [ ] **Step 2: Complete protocol/transport through native and build-only gates**

```bash
/home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc twister \
  -T tests/subsys/mpipe/ipc_protocol -p native_sim/native/64 -p qemu_cortex_m3
/home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc twister \
  -T tests/subsys/mpipe/ipc_transport -p native_sim/native/64
```

Continue to real two-core tests only when the clean probe passed and the exact
new firmware hashes are approved.

- [ ] **Step 3: Complete mpipe/audio/inference in ascending evidence order**

Require native element ownership tests, then synthetic real transport, then
DMIC metrics, then fixed reference model fixtures, then transported fixtures,
then live speech. Keep optimization in its own optional commit and gate.

- [ ] **Step 4: Complete Linux integration and the final matrix**

The production Linux DT and supervisor must pass their host-side tests before
installation. Final acceptance requires the full fresh-build, dual-core,
restart, pressure, soak, attribution, DCO, and repository-hygiene matrix in the
Linux integration plan.

### Task 3: Final handoff without external mutation

**Files:**
- Modify: `/home/mt/zephyrproject/review/PROGRESS.md`
- Create: `/home/mt/zephyrproject/review/hardware-logs/2026-09-11-imx8mp-m7-hifi4-final.log`

**Interfaces:**
- Consumes: all plan outputs and evidence.
- Produces: a review packet; it does not push or merge anything.

- [ ] **Step 1: Run final verification commands fresh**

```bash
set -euo pipefail
cd /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc
ipc_plan_commit=$(git merge-base HEAD codex/imx8mp-m7-hifi4-ipc-design)
git diff --check "$ipc_plan_commit"..HEAD
git log --format='%h %an <%ae> %s%n%b' "$ipc_plan_commit"..HEAD
git status --short
/home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc twister -T tests/subsys/mpipe
```

- [ ] **Step 2: Prove only intended files and authorship are present**

```bash
set -euo pipefail
cd /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc
ipc_plan_commit=$(git merge-base HEAD codex/imx8mp-m7-hifi4-ipc-design)
git diff --name-status "$ipc_plan_commit"..HEAD
git log --format='%H%x09%an%x09%ae%x09%s' -- \
  samples/modules/tflite-micro/micro_speech
find . -type f \( -name '*.elf' -o -name '*.bin' -o -name '*.dtb' -o -name '*.log' \) -print
```

Expected: no generated artifact is tracked, and attributed model commits name
Thong Phan with retained TensorFlow license headers.

- [ ] **Step 3: Present the review packet and stop**

Report branch heads, commit authors, every test/build/hardware result with its
classification, remaining limitations, and exact diff. Ask separately for any
merge, push, upstream submission, or destructive cleanup.
