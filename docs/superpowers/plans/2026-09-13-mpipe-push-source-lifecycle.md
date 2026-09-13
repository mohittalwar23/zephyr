# Mpipe Push-Source Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give externally driven mpipe sources deterministic initialization, state-driven activation, and a callback admission/drain barrier without allocating a useless pipeline thread.

**Architecture:** The base source owns an atomic admission gate, an in-flight delivery count, and a semaphore used only by the thread performing deactivation. A push callback enters the gate before touching delivery resources and leaves it afterward; `PLAYING -> PAUSED` closes admission and waits for accepted callbacks before invoking the derived source's deactivate hook. Pull sources keep their current pipeline-thread behavior, while push sources allocate no pipeline thread.

**Tech Stack:** Zephyr C, atomics, semaphores, mpipe state machine, ztest, native_sim.

**Spec:** `docs/superpowers/specs/2026-09-13-mpipe-ipc-correctness-design.md`, section “2. Mpipe externally driven source lifecycle”.

## Global Constraints

- Develop each behavior test-first and observe its failure against the preceding production revision.
- Existing sources default to `MPIPE_SRC_DRIVE_PULL` even when their storage contains nonzero bytes before initialization.
- A push source accepts deliveries only while its pipeline is `PLAYING`.
- Closing admission must prevent new delivery entries and wait until every accepted delivery has left.
- Do not hold a lock across a downstream mpipe call; the gate uses atomics and a semaphore notification.
- A rejected delivery remains owned by its caller; the derived source decides how to return it to its producer.
- Activation/deactivation hooks run only for push-driven sources.
- Pull-driven source scheduling and pause/replay behavior must not change.
- Do not modify the IPC plugin in this change; it will adopt the lifecycle API together with its per-instance ownership and endpoint teardown work.
- Preserve and do not stage concurrent work outside the files named by each task.

---

### Task 1: Deterministic base-source initialization

**Files:**

- Create: `tests/subsys/mpipe/unit/src/test_src.c`
- Modify: `subsys/mpipe/mpipe_src.c`

**Interfaces:**

- Consumes: `mpipe_src_init(struct mpipe_src *src, uint8_t id)`.
- Produces: a source whose scheduling and ownership fields have deterministic defaults independent of prior storage contents.

- [ ] **Step 1: Add a regression test initialized from nonzero storage**

Create a `mpipe_src_api` ztest suite. Fill a local `struct mpipe_src` with `0xa5`, call `mpipe_src_init()`, and assert these hand-derived defaults:

```c
zassert_equal(src.drive, MPIPE_SRC_DRIVE_PULL);
zassert_is_null(src.pool);
zassert_equal(src.num_buffers, 0U);
zassert_is_null(src.decide_buffer_pool);
```

Do not assert callbacks that `mpipe_src_init()` deliberately installs, such as `set_caps`.

- [ ] **Step 2: Run the unit suite and verify RED**

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
west twister -T tests/subsys/mpipe/unit -p native_sim/native/64 \
  --inline-logs --outdir /tmp/codex-mpipe-src-init-red
```

Expected: `mpipe_src_api.test_init_overwrites_nonzero_scheduling_state` fails because at least `drive`, `pool`, or `num_buffers` retains `0xa5` bytes.

- [ ] **Step 3: Initialize every existing source-owned field**

In `mpipe_src_init()`, after the base element and pad setup, explicitly assign:

```c
src->pool = NULL;
src->drive = MPIPE_SRC_DRIVE_PULL;
src->num_buffers = 0U;
src->decide_buffer_pool = NULL;
```

Keep the installed base callbacks unchanged.

- [ ] **Step 4: Run the unit suite and verify GREEN**

Run the Task 1 Twister command with outdir `/tmp/codex-mpipe-src-init-green` and require every selected test case to pass with no warnings.

- [ ] **Step 5: Commit the isolated initialization fix**

```bash
git add subsys/mpipe/mpipe_src.c tests/subsys/mpipe/unit/src/test_src.c
git diff --cached --check
git commit -m "mpipe: initialize source scheduling state"
```

### Task 2: State-driven push-source callback barrier

**Files:**

- Modify: `include/zephyr/mpipe/mpipe_src.h`
- Modify: `subsys/mpipe/mpipe_src.c`
- Modify: `tests/subsys/mpipe/unit/src/test_src.c`

**Interfaces:**

- Produces: `bool mpipe_src_delivery_enter(struct mpipe_src *src)` and `void mpipe_src_delivery_leave(struct mpipe_src *src)`.
- Produces: optional `int (*activate)(struct mpipe_src *src)` and `int (*deactivate)(struct mpipe_src *src)` hooks.
- Consumes: `MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING` and `MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED` in `mpipe_src_change_state()`.

- [ ] **Step 1: Add lifecycle and failure tests before the API exists**

Extend `test_src.c` with hook counters and tests covering:

```c
/* Gate is closed after init and throughout READY/PAUSED. */
zassert_false(mpipe_src_delivery_enter(&src));

/* PAUSED -> PLAYING activates exactly once and opens admission. */
zassert_equal(mpipe_src_change_state(&src.element,
                                    MPIPE_STATE_CHANGE_PAUSED_TO_PLAYING),
              MPIPE_STATE_CHANGE_SUCCESS);
zassert_true(mpipe_src_delivery_enter(&src));
mpipe_src_delivery_leave(&src);

/* PLAYING -> PAUSED closes admission before deactivate runs. */
zassert_equal(mpipe_src_change_state(&src.element,
                                    MPIPE_STATE_CHANGE_PLAYING_TO_PAUSED),
              MPIPE_STATE_CHANGE_SUCCESS);
zassert_false(mpipe_src_delivery_enter(&src));
```

Also test that an activation hook returning `-EIO` makes the transition fail and leaves admission closed, and that a deactivation hook returning `-EIO` makes the transition fail while admission remains closed.

- [ ] **Step 2: Run the unit suite and verify RED**

Run the Task 1 Twister command with outdir `/tmp/codex-mpipe-src-lifecycle-red`.

Expected: compilation fails because the lifecycle hooks and delivery entry/leave API do not exist. This failure is the public API contract being introduced, not a test typo.

- [ ] **Step 3: Add the base lifecycle state and API**

Add these source-owned fields, initialized by `mpipe_src_init()`:

```c
int (*activate)(struct mpipe_src *src);
int (*deactivate)(struct mpipe_src *src);
atomic_t delivery_active;
atomic_t deliveries_in_flight;
struct k_sem deliveries_drained;
```

`mpipe_src_delivery_enter()` must:

1. return false for NULL or a closed gate;
2. increment `deliveries_in_flight` after the first active check;
3. recheck the gate to close the race with deactivation;
4. if the recheck is closed, decrement the counter, signal `deliveries_drained` when the old count was one, and return false;
5. otherwise return true.

`mpipe_src_delivery_leave()` decrements the in-flight counter and gives `deliveries_drained` when the old count was one and admission is closed. It must never block, so it remains usable from callback context.

The internal drain operation atomically closes admission and loops while the in-flight count is nonzero, taking `deliveries_drained` with `K_FOREVER`. The loop is required because a callback that loses the admission race can leave a stale semaphore token after an earlier deactivation has already returned.

- [ ] **Step 4: Drive hooks from source state transitions**

For a push-driven source only:

- on `PAUSED -> PLAYING`, open admission before calling `activate` so a source that delivers synchronously from its activation hook is accepted; if activation fails, close and drain the gate and return `MPIPE_STATE_CHANGE_FAILURE`;
- on `PLAYING -> PAUSED`, close and drain admission before calling `deactivate`; if the hook fails, keep admission closed and return `MPIPE_STATE_CHANGE_FAILURE`.

Pull-driven sources must not call either hook.

- [ ] **Step 5: Add a deterministic callback/deactivation race test**

Use two ztest worker threads and semaphores:

1. activate a push source;
2. worker A enters delivery, signals `entered`, and blocks on `release`;
3. worker B performs `PLAYING -> PAUSED` and signals `deactivated` on return;
4. assert `deactivated` is not available before `release`;
5. assert a new `mpipe_src_delivery_enter()` is rejected while worker B waits;
6. release worker A, then require worker B to complete and the deactivate hook to have observed a closed gate.

The test catches any implementation that closes the gate after invoking the hook, fails to wait for accepted callbacks, or admits a new callback during the drain.

- [ ] **Step 6: Run the unit suite and verify GREEN**

Run the Task 1 Twister command with outdir `/tmp/codex-mpipe-src-lifecycle-green` and require every test to pass with no warnings.

- [ ] **Step 7: Commit the lifecycle barrier**

```bash
git add include/zephyr/mpipe/mpipe_src.h subsys/mpipe/mpipe_src.c \
  tests/subsys/mpipe/unit/src/test_src.c
git diff --cached --check
git commit -m "mpipe: add push-source lifecycle barrier"
```

### Task 3: Do not allocate a pipeline thread for a push source

**Files:**

- Modify: `include/zephyr/mpipe/mpipe_pipeline.h`
- Modify: `subsys/mpipe/mpipe_pipeline.c`
- Modify: `tests/subsys/mpipe/pipeline/src/test_mock_pipeline.c`

**Interfaces:**

- Consumes: `mpipe_src.drive` and the existing `MPIPE_SRC_DRIVE_PULL` / `MPIPE_SRC_DRIVE_PUSH` distinction.
- Produces: unchanged pipeline state behavior with thread operations performed only for pull-driven sources.

- [ ] **Step 1: Add a two-pipeline resource test**

With the existing default `CONFIG_MPIPE_THREADS_NUM=1`, construct two independent pipelines, each containing a fake source with `src.drive = MPIPE_SRC_DRIVE_PUSH` and a base sink. Link both graphs, then move both pipelines to `MPIPE_STATE_PLAYING`.

```c
zassert_equal(mpipe_element_set_state(&pipe_a.bin.element, MPIPE_STATE_PLAYING),
              MPIPE_STATE_CHANGE_SUCCESS);
zassert_equal(mpipe_element_set_state(&pipe_b.bin.element, MPIPE_STATE_PLAYING),
              MPIPE_STATE_CHANGE_SUCCESS);
```

Return both to `MPIPE_STATE_READY`. The test is behavioral: two externally driven pipelines need zero pipeline stack slots and therefore can coexist when only one pull-thread slot is configured.

- [ ] **Step 2: Run the pipeline suite and verify RED**

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
west twister -T tests/subsys/mpipe/pipeline -p native_sim/native/64 \
  --inline-logs --outdir /tmp/codex-mpipe-push-thread-red
```

Expected: the second transition fails because the first push source consumed the only mpipe thread stack with its parked dummy thread.

- [ ] **Step 3: Make pipeline thread ownership conditional on source drive**

Extract the existing child scan into a private `mpipe_pipeline_find_src()` helper and use it from both the thread entry and `mpipe_pipeline_change_state()`.

For `MPIPE_SRC_DRIVE_PUSH`, skip:

- `mpipe_thread_create()` on `READY -> PAUSED`;
- `mpipe_thread_resume()` on `PAUSED -> PLAYING`;
- `mpipe_thread_pause()` on `PLAYING -> PAUSED`;
- `mpipe_thread_join()` on `PAUSED -> READY`.

Keep child transition ordering, pad flushing, sink counting, and EOS reset unchanged. Update `mpipe_pipeline.h` to state that the top-level thread exists only for pull-driven sources.

- [ ] **Step 4: Run focused and broader suites**

Run the Task 3 command with outdir `/tmp/codex-mpipe-push-thread-green`, then run:

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
west twister -T tests/subsys/mpipe -p native_sim/native/64 \
  --inline-logs --outdir /tmp/codex-mpipe-src-lifecycle-all
```

Require all executed mpipe tests to pass. If unrelated dirty audio files cause a failure, diagnose it and report it separately; do not alter or stage those files.

- [ ] **Step 5: Build both real consumers**

Build `samples/subsys/mpipe/ipc_bringup` and `samples/subsys/mpipe/ipc_infer` for `imx8mp_evk/mimx8ml8/m7` and `imx8mp_evk/mimx8ml8/adsp`, using fresh `/tmp` directories and the inference configuration from the fault-barrier build. This proves existing pull sources still compile and the current IPC push source compiles against the expanded base type.

- [ ] **Step 6: Style-check and commit the isolated thread change**

```bash
git diff --check -- include/zephyr/mpipe/mpipe_pipeline.h \
  subsys/mpipe/mpipe_pipeline.c tests/subsys/mpipe/pipeline/src/test_mock_pipeline.c
git diff -- include/zephyr/mpipe/mpipe_pipeline.h subsys/mpipe/mpipe_pipeline.c \
  tests/subsys/mpipe/pipeline/src/test_mock_pipeline.c | \
  scripts/checkpatch.pl --no-tree -
git add include/zephyr/mpipe/mpipe_pipeline.h subsys/mpipe/mpipe_pipeline.c \
  tests/subsys/mpipe/pipeline/src/test_mock_pipeline.c
git diff --cached --check
git commit -m "mpipe: skip pipeline threads for push sources"
```

## Completion gate

Completion requires three observed RED/GREEN cycles, deterministic initialization from nonzero storage, a race test proving deactivation blocks until accepted callbacks leave while rejecting new callbacks, two simultaneous push pipelines with one configured stack, the full native mpipe suite, both target consumer builds, zero checkpatch errors/warnings, and three commits containing only the files named in their respective tasks.
