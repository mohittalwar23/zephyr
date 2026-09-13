# Static Vrings Close Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep an open static-vrings IPC Service instance usable and closable after `ipc_service_close_instance()` refuses to close because endpoints remain registered.

**Architecture:** Extend the existing cross-core IPC Service API test so it first proves a refused close does not poison the instance and then proves normal deregistration and close still succeed. Fix the backend at the state transition itself by restoring `STATE_INITED` before returning `-EBUSY`, matching the established RPMsg-Lite backend behavior.

**Tech Stack:** Zephyr C, IPC Service, OpenAMP static vrings, ztest, sysbuild, Twister, BabbleSim nRF5340.

**Spec:** `docs/superpowers/specs/2026-09-13-mpipe-ipc-correctness-design.md`, section “1. IPC Service backend close semantics”.

## Global Constraints

- Develop test-first and observe the new test fail for the expected `-EBUSY` state-poisoning reason before editing production code.
- Change no public API and no devicetree or Kconfig contract.
- Keep the fix independent of mpipe and the i.MX8MP integration.
- Restore `STATE_INITED` only when close refuses before teardown begins; retain the existing error handling for failures after deinitialization starts.
- Preserve and do not stage the unrelated modifications in `drivers/audio/codec_dummy.c`, `subsys/mpipe/aud/Kconfig`, `subsys/mpipe/aud/mpipe_aud_buffer_pool.c`, and the two untracked XIAO board files.

---

### Task 1: Reproduce and fix retry after endpoint-caused `-EBUSY`

**Files:**

- Modify: `tests/subsys/ipc/ipc_service_api/src/main.c:257-325,909`
- Modify: `subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c:628-645`

**Interfaces:**

- Consumes: `ipc_service_close_instance(const struct device *instance)`, `ipc_service_deregister_endpoint(struct ipc_ept *ept)`, the existing `test_echo(struct test_context *ctx)` helper, and the `CONFIG_IPC_SERVICE_BACKEND_RPMSG` static-vrings test configuration.
- Produces: unchanged IPC Service API behavior in which a refused close returns `-EBUSY` while leaving the instance in `STATE_INITED`, so endpoint operations and a later close remain valid.

- [ ] **Step 1: Add a failing regression test for usability after refused close**

Add this test to `tests/subsys/ipc/ipc_service_api/src/main.c` after the existing endpoint tests. It deliberately sends an echo after the refused close; against the current backend that send returns `-EBUSY` because the backend was left in `STATE_BUSY`.

```c
ZTEST(ipc_service_api, test_close_with_registered_endpoint_keeps_instance_usable)
{
	int ret;

	Z_TEST_SKIP_IFNDEF(CONFIG_IPC_SERVICE_BACKEND_RPMSG);

	ret = ipc_service_close_instance(ipc_instance);
	zassert_equal(ret, -EBUSY,
		      "Close with a registered endpoint should return -EBUSY, got %d", ret);

	test_echo(ep0());
}
```

Add a suite teardown helper that proves the restored instance can subsequently release all local endpoints and close. The remote image may remain alive; the backend close contract requires only the local instance's endpoints to be deregistered.

```c
static void suite_teardown(void *fixture)
{
	size_t ep_cnt = IS_ENABLED(CONFIG_IPC_SERVICE_API_TEST_SECOND_ENDPOINT) ? 2 : 1;
	int ret;

	ARG_UNUSED(fixture);

	if (!IS_ENABLED(CONFIG_IPC_SERVICE_BACKEND_RPMSG)) {
		return;
	}

	for (size_t i = 0; i < ep_cnt; i++) {
		ret = ipc_service_deregister_endpoint(&test_ctx[i].ep);
		zassert_ok(ret, "Endpoint %zu deregistration failed: %d", i, ret);
	}

	ret = ipc_service_close_instance(ipc_instance);
	zassert_ok(ret, "Close after endpoint deregistration failed: %d", ret);
}
```

Pass `suite_teardown` as the final callback:

```c
ZTEST_SUITE(ipc_service_api, NULL, suite_setup, suite_before, NULL, suite_teardown);
```

- [ ] **Step 2: Build and run the static-vrings scenario to verify RED**

Run:

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
west twister \
  -T tests/subsys/ipc/ipc_service_api \
  -p nrf5340bsim/nrf5340/cpuapp \
  -s tests.ipc.ipc_service_api.static_vrings \
  --inline-logs \
  --outdir /tmp/codex-static-vrings-close-red
```

Expected: the new test fails when `test_echo()` reports an IPC send result of `-EBUSY`. If the environment cannot execute BabbleSim, build the scenario and preserve the infrastructure error separately; do not count a build-only result as the required RED observation.

- [ ] **Step 3: Restore the backend state before returning `-EBUSY`**

Change only the endpoint-presence branch in `subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c`:

```c
	if (!check_endpoints_freed(rpmsg_inst)) {
		/* Restore state: endpoints are still active, so the instance remains open. */
		atomic_set(&data->state, STATE_INITED);
		return -EBUSY;
	}
```

Do not move teardown calls or alter the existing `error:` path. At this point no resources have been deinitialized, so `STATE_INITED` accurately describes the instance.

- [ ] **Step 4: Re-run the focused scenario to verify GREEN**

Run:

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
west twister \
  -T tests/subsys/ipc/ipc_service_api \
  -p nrf5340bsim/nrf5340/cpuapp \
  -s tests.ipc.ipc_service_api.static_vrings \
  --inline-logs \
  --outdir /tmp/codex-static-vrings-close-green
```

Expected: `tests.ipc.ipc_service_api.static_vrings` passes, including the new refused-close echo and final deregister/close assertions.

- [ ] **Step 5: Run the broader IPC Service API regression matrix**

Build or run all integration platforms available in the local environment:

```bash
PATH=/home/mt/zephyrproject/.venv/bin:$PATH \
ZEPHYR_BASE=/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc \
west twister \
  -T tests/subsys/ipc/ipc_service_api \
  --integration \
  --inline-logs \
  --outdir /tmp/codex-ipc-service-api-regression
```

Expected: every runnable scenario passes. Report unavailable hardware or simulator dependencies as filtered/blocked rather than silently reducing the matrix.

- [ ] **Step 6: Check formatting and review the exact patch**

Run:

```bash
git diff --check -- \
  tests/subsys/ipc/ipc_service_api/src/main.c \
  subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c
git diff -- \
  tests/subsys/ipc/ipc_service_api/src/main.c \
  subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c | \
  scripts/checkpatch.pl --no-tree -
git diff -- \
  tests/subsys/ipc/ipc_service_api/src/main.c \
  subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c
```

Expected: no whitespace or checkpatch errors. Confirm the diff contains only the regression test, teardown verification, one state restoration, and explanatory comments.

- [ ] **Step 7: Commit only the isolated fix**

```bash
git add \
  tests/subsys/ipc/ipc_service_api/src/main.c \
  subsys/ipc/ipc_service/backends/ipc_rpmsg_static_vrings.c
git diff --cached --check
git diff --cached --name-status
git commit -m "ipc: static_vrings: restore state after refused close"
```

Expected staged paths: exactly the two files listed above. The commit message describes the user-visible behavior, and all unrelated working-tree files remain unstaged.

## Completion gate

This plan is complete only when the focused test has been observed failing before the backend edit and passing afterward, the broader runnable IPC Service API matrix has no failures, the exact two-file staged diff passes formatting checks, and the commit contains no mpipe or board-specific dependency.
