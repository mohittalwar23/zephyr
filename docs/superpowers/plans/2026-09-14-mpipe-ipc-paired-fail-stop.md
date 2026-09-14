# Mpipe IPC Paired Fail-Stop Implementation Plan

> **Implementation rule:** develop each behavior test-first and preserve the
> unrelated audio and XIAO work already present in the worktree.

**Goal:** Make the direct M7/HiFi4 mpipe link reject stale traffic and tear down
both IPC endpoints safely after either core restarts, leaving Linux to reset the
pair.

**Architecture:** Each IPC plugin element is tied to the transport session that
owns its IPC Service instance. Receive admission is closed and drained before
endpoint deregistration. DATA/RELEASE carry the zero-copy owner's session, while
the sink quarantines any still-outstanding pool references until paired reset.

**Tech stack:** Zephyr IPC Service, mpipe, net_buf, ztest, native_sim, NXP
i.MX8MP Cortex-M7 and HiFi4 targets.

---

## Task 1: Specify and test the plugin lifecycle ABI

**Files:**

- Modify: `include/zephyr/mpipe/ipc/mpipe_ipc_plugin.h`
- Modify: `tests/subsys/mpipe/ipc_plugin/src/main.c`

1. Add a fake transport with a live negotiated session to the plugin test.
2. Add tests showing stale-session callbacks do not deliver DATA or consume a
   RELEASE.
3. Add a test showing an old generation cannot release a reused numeric slot.
4. Add a test showing source wrappers from two instances return on the endpoint
   that accepted them.
5. Add tests for deinit idempotence and endpoint deregistration.
6. Add tests for exact-once retry claims, malformed CAPS, stale transport
   RELEASE, sink teardown races, and buffer-pool demand.
7. Run the focused native suite and confirm the new tests fail for the missing
   APIs or behavior.

## Task 2: Implement generation-safe plugin ownership

**Files:**

- Modify: `include/zephyr/mpipe/ipc/mpipe_ipc_msg.h`
- Modify: `include/zephyr/mpipe/ipc/mpipe_ipc_plugin.h`
- Modify: `subsys/mpipe/ipc/mpipe_ipc_plugin_src.c`
- Modify: `subsys/mpipe/ipc/mpipe_ipc_plugin_sink.c`

1. Require a live `mpipe_ipc_transport` when initializing a plugin element.
2. Validate it before parsing every receive callback.
3. Carry the sender's local session in DATA and echo it in RELEASE.
4. Record a generation alongside every sink pending slot and require an exact
   match before dropping the reference.
5. Replace the global wrapper owner with a per-source wrapper-pool object and
   track live IDs per source.
6. Add delayed, per-source release retry and cancel it synchronously during
   teardown.
7. Add source and sink deinit APIs that close callback admission, drain admitted
   callbacks, and deregister the endpoint.
8. Use one atomic closed/count word so admission and teardown have a single
   linearization point.
9. Claim queued RELEASE records before sending, restoring a claim only after a
   failed send.
10. Validate received CAPS before storing or dispatching them, and propose the
    sink's outstanding-buffer demand upstream.
11. Re-run the focused native suite until it passes.

## Task 3: Wire the sample's terminal teardown

**Files:**

- Modify: `samples/subsys/mpipe/ipc_infer/src/producer.h`
- Modify: `samples/subsys/mpipe/ipc_infer/src/consumer.h`
- Modify: `samples/subsys/mpipe/ipc_infer/src/pipeline.c`
- Modify: `samples/subsys/mpipe/ipc_infer/src/consumer.c`
- Modify: `samples/subsys/mpipe/ipc_infer/src/main.c`
- Modify: `samples/subsys/mpipe/ipc_infer/README.rst`
- Modify: `include/zephyr/mpipe/ipc/mpipe_ipc_transport.h`
- Modify: `subsys/mpipe/ipc/mpipe_ipc_transport.c`
- Modify: `tests/subsys/mpipe/ipc_protocol/src/test_transport.c`

1. Pass the transport into the plugin element during pipeline construction.
2. Add producer/consumer stop functions that deinitialize the player and then
   the audio endpoint.
3. On peer restart, stop the role pipeline, deregister `mpipe.audio`, deregister
   `mpipe.ctrl`, and only then quiesce the transport.
4. Propagate and publish the first teardown error; never call local rebuild.
5. Remain stopped after successful teardown so Linux can perform paired reset.
6. Ensure partial startup also reaches the same role teardown path.

## Task 4: Verify and review

1. Run `west twister -T tests/subsys/mpipe/ipc_plugin -p native_sim/native/64`.
2. Run `west twister -T tests/subsys/mpipe/ipc_protocol -p native_sim/native/64`.
3. Run the complete native mpipe test set.
4. Build `samples/subsys/mpipe/ipc_infer` for
   `imx8mp_evk/mimx8ml8/m7` with `live_i2s.conf` plus
   `live_i2s.overlay`, and for `imx8mp_evk/mimx8ml8/adsp`.
5. Inspect the diff for callback lifetime, exact-send handling, reference
   ownership, and endpoint-close ordering.
6. Stage only the files named in this plan and commit the coherent change.
