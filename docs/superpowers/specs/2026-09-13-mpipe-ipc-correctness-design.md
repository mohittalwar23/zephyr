# Mpipe IPC Correctness and Recovery Design

## Context

The direct Cortex-M7 to HiFi4 audio path proves that mpipe buffers can cross an
IPC Service static-vrings transport without copying the PCM payload. The current
implementation is safe only during steady-state operation. Its local rebuild
path leaves the audio endpoint registered, data-plane messages are not tied to
a peer incarnation, and peer-provided buffer descriptors are trusted before
memory is accessed.

This design turns the prototype into a sequence of independently reviewable
changes. It deliberately chooses paired recovery for the first correct version:
either core may detect the fault, but Linux remains responsible for restarting
the pair. Independent one-core recovery is deferred until ownership draining is
specified and implemented end to end.

## Goals

- Make an unsuccessful IPC Service close retryable instead of permanently
  wedging the static-vrings backend.
- Give externally driven mpipe sources complete activate, deactivate, and
  teardown semantics.
- Stop callbacks and retire buffer ownership before deregistering endpoints or
  publishing a quiescent state.
- Validate the transport session before delivering any plugin message, and
  generation-scope zero-copy DATA/RELEASE ownership.
- Reject malformed or out-of-region payload descriptors before cache
  maintenance or memory access.
- Make source and sink state per-instance and safe across callback, pipeline,
  destructor, and retry contexts.
- Provide deterministic native tests for failure, restart, ownership, and wire
  compatibility, followed by targeted hardware tests.
- Preserve the working live-audio demonstration while separating generic
  subsystem changes from i.MX8MP platform integration.

## Non-goals

- Transparent recovery after restarting only one core.
- Recovering buffers still owned by a crashed peer.
- Treating shared memory alone as a lifecycle authority.
- Defining Linux remoteproc policy in the generic Zephyr IPC or mpipe APIs.
- Upstreaming the full integration branch as one change.

## Approach considered

### Paired recovery first — selected

The survivor detects a session change, stops all local producers and consumers,
retires locally held references, deregisters endpoints, and closes its IPC
instance. It publishes `DOWN` only after those operations succeed. Linux then
restarts both cores. This matches the existing system-level lifecycle policy and
does not pretend that ownership held by a crashed core can be recovered safely.

### Independent one-core recovery

This would retain the current `rebuild()` intent but requires a generation in
every message, cancellation and draining for all outstanding references, rules
for reclaiming memory owned by the crashed core, and coordination that prevents
ring reset while the survivor has callbacks in flight. It remains a future
extension after the paired path is proven.

### Detection-only patching

Only fixing the backend state and descriptor validation would reduce immediate
damage but leave the sample advertising a recovery path that cannot work. This
is not sufficient as an end state.

## Architecture

The work is split into five layers whose changes can be reviewed and tested
independently.

### 1. IPC Service backend close semantics

`ipc_rpmsg_static_vrings_close_instance()` temporarily claims `STATE_BUSY` to
serialize close. If registered endpoints make close return `-EBUSY`, it must
restore `STATE_INITED` before returning. No endpoint or mpipe dependency belongs
in this fix.

A backend unit test shall open an instance, register an endpoint, attempt close,
observe `-EBUSY`, deregister the endpoint, and then successfully close the same
instance. The regression test must fail against the current implementation
because the first close leaves the backend busy.

### 2. Mpipe externally driven source lifecycle

The source abstraction shall support callbacks that activate and deactivate the
source when pipeline state enters and leaves `PLAYING`. Pull-driven sources keep
their worker-thread behavior. Externally driven sources do not receive a dummy
parked worker thread.

The base initializer must set every scheduling/lifecycle field explicitly, so a
stack-allocated object filled with nonzero bytes behaves deterministically after
initialization. A source may push only while active. Deactivation must prevent
new pushes and wait for any callback already inside the delivery critical
section before returning.

Tests shall cover transitions through `READY`, `PAUSED`, `PLAYING`, and `NULL`,
including an arrival racing deactivation.

### 3. Plugin session, wire, and memory contract

The plugin is bound to the transport that owns its IPC Service instance. Every
receive callback checks that transport's current peer session before parsing or
acting on a plugin message. This follows the same ordering as IPC Service's
ICMsg backend: detect a changed session and disconnect before using a queued
message from the old incarnation.

The current fixed-size plugin message remains versioned. A future change may
replace the C structure with an explicit codec, but that wire cleanup is not a
prerequisite for paired fail-stop recovery and is not mixed into the lifecycle
patch. Unlike the earlier draft of this design, CAPS and EVENT are not given a
redundant per-message session tuple: transport validation already supplies that
authority.

DATA carries a payload-region offset, size, buffer identifier, timestamp, and
the sender's local session as the ownership generation. RELEASE echoes that
generation. The generation is checked against the outstanding slot before its
reference is dropped, preventing a delayed release from freeing a buffer after
the same numeric identifier has been reused.

The plugin instance receives a shared-region description containing its local
mapping, region size, and required alignment. Both cores use offsets from the
same physical shared-window origin. Before
allocating a wrapper, invalidating cache, or dereferencing memory, receive code
must prove without integer overflow that:

- the transport still recognizes the peer session;
- a DATA ownership generation is the transport's negotiated remote session;
- the buffer identifier is inside the configured window;
- the offset is inside the region;
- the size is nonzero and no larger than the remaining region;
- offset and size satisfy the negotiated alignment and audio-format rules.

Unknown versions, message types, and short or trailing messages are rejected.
Send succeeds only when IPC Service reports the exact message byte count.

### 4. Per-instance ownership and reliable release

Global wrapper ownership is removed. A common allocation pool may supply the
small wrapper objects, but each wrapper's `mpipe_buffer_pool` points back to the
source instance that accepted it. Each source owns its release and live-ID
bitmaps, endpoint configuration, synchronization primitive, retry work,
counters, and negotiated state.

One DATA identifier represents at most one live ownership record in a given
generation. A wrapper destructor queues one RELEASE exactly once. The mpipe push
API consumes the caller's reference on both success and failure; the IPC source
must not unref it a second time.

The release set scales to `CONFIG_MPIPE_IPC_PLUGIN_MAX_BUFFERS` using a bit array
rather than a 32-bit scalar. A failed send schedules delayed retry independently
of future receive callbacks. Teardown synchronously cancels retry work after
preventing new destructors from targeting the endpoint. Entries from an old
generation are never sent to a new peer.

The sink proposes its outstanding-buffer requirement through mpipe buffer-pool
negotiation. Kconfig remains a capacity ceiling, not an unenforced claim that an
upstream pool has a particular size.

### 5. Paired fault and teardown coordinator

The transport state exposed to the peer distinguishes these conditions:

- `READY`: endpoint registration and data-plane use are allowed;
- `DOWN`: all endpoints are deregistered and the instance is closed;
- `FAULT`: teardown or close failed, so the peer must not reset shared rings.

The current control block keeps a single writer for each naturally aligned
32-bit field and publishes the session before the state. A sequence-counter
snapshot format is deferred together with any future wire-format revision; the
paired fail-stop patch does not need a transient shared `STOPPING` state because
the survivor remains `FAULT` until every local teardown step and close succeed.

When a peer generation change is detected, the sample closes plugin admission,
stops and joins its pipeline worker, cancels release retry work, deregisters the
audio endpoint and then the control endpoint, and closes the instance. A
successful close publishes `DOWN`; any failure publishes `FAULT` and asks the
Linux lifecycle authority for paired recovery. The Zephyr process does not call
local `rebuild()` or touch the shared rings again in this version.

Sink buffers whose DATA was delivered but whose matching RELEASE did not arrive
stay referenced in their slots. They are intentionally quarantined until the
paired reset; a peer session change is not proof that the failed peer stopped
reading shared memory.

## Concurrency and ownership rules

- Endpoint callbacks may enter concurrently with pipeline state changes and
  endpoint teardown.
- Per-instance mutable state is protected by a lock or an explicitly documented
  atomic protocol; plain `volatile` is not synchronization.
- No plugin lock is held across `ipc_service_send()` or downstream mpipe calls.
- Deactivation first closes the admission gate, then waits for active callbacks,
  then drains ownership.
- The sender owns a pool buffer until it receives one valid RELEASE for the same
  identifier and generation.
- The receiver owns one wrapper reference after accepting DATA; mpipe push
  consumes that reference regardless of its return value.
- Endpoint deinitialization closes callback admission and waits for callbacks
  already admitted before deregistering the endpoint.
- A generation change invalidates protocol traffic but does not by itself prove
  that memory owned by the previous generation is safe to reuse. Paired restart
  supplies that lifecycle boundary.

## Error handling

- Malformed control or plugin messages increment diagnostics and are discarded
  without accessing the described payload.
- `-ENOMEM` or temporary IPC transmit exhaustion schedules bounded retry where
  retry is safe; it does not silently discard RELEASE.
- Endpoint deregistration and close errors are propagated to the coordinator.
- `DOWN` is never published on partial teardown.
- A timeout while waiting for a required peer becomes `FAULT`; it is not treated
  as evidence that a live peer is absent.
- Hardware integration logs include generation, state, outstanding ownership,
  and the first teardown error without relying on fixed private DDR offsets.

## Test strategy

Each production behavior is developed test-first, demonstrating the regression
against the preceding revision before adding the minimal implementation.

Native coverage includes:

- retrying close after an endpoint-caused `-EBUSY`;
- explicit source initialization from nonzero memory;
- no pushes outside `PLAYING` and no callback surviving teardown;
- rejection of incompatible versions/layouts;
- short sends and transient send exhaustion;
- buffer IDs 0, 31, 32, and 63;
- two simultaneous source instances returning wrappers to their own endpoint;
- address underflow, overflow, oversize, misalignment, and invalid IDs;
- exactly-once release after successful push, failed push, unlink, and teardown;
- failed final RELEASE while the peer is otherwise quiet;
- stale DATA and RELEASE generations, including reuse of the same numeric ID;
- endpoint deinitialization racing an admitted callback;
- peer-generation change while sink slots or source wrappers remain occupied;
- `DOWN` only after complete teardown and `FAULT` after any teardown failure.

Target builds cover both `imx8mp_evk/mimx8ml8/m7` and
`imx8mp_evk/mimx8ml8/adsp`, including a 64-buffer configuration. Hardware tests
then exercise the live-audio pair under pressure and each one-core failure.
Automated Linux paired-reset supervision and long soak coverage remain
integration follow-ups rather than generic mpipe behavior.

## Delivery sequence

1. Submit the static-vrings close-state regression and fix independently.
2. Agree the asynchronous-source lifecycle on the active mpipe work.
3. Add paired fault semantics without local rebuild.
4. Replace the plugin ABI and globals with the validated per-instance design.
5. Wire complete teardown into the i.MX8MP live-audio/inference consumer, with
   its Linux remoteproc prerequisites documented separately.

Each change remains buildable and testable at its own boundary. The existing
uncommitted audio-pool sizing and XIAO board files are outside this design and
must be preserved untouched while these changes are developed.
