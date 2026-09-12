# i.MX8MP MU3 Protocol and Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Do not delegate implementation unless the user explicitly authorizes it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement and prove the version-1 direct M7-to-HiFi4 wire protocol, retained-generation bootstrap, and one MU3/static-vrings transport with two coordinated logical endpoints.

**Architecture:** Architecture-neutral codecs and state machines sit above a narrow IPC Service adapter. One physical static-vrings instance owns both control and audio endpoints, all close/fault behavior, and the shared credit ledger. Board overlays provide the complementary MU3 channel mapping only after the clean probe has passed.

**Tech Stack:** Zephyr C, ztest, native_sim, qemu_cortex_m3, IPC Service static-vrings/OpenAMP, NXP i.MX MBOX/MU, devicetree, CRC32C.

**Spec:** `docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`

## Entry gate and constraints

- The hardware-probe plan must have a green evidence record for MU3 access, channels, IRQ7 acknowledgement, DDR visibility, cache attributes, MU2 READY, clock lifetime, and rollback.
- Work only in `/home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc` on `codex/imx8mp-m7-hifi4-ipc`; never merge the diagnostic branch wholesale.
- Keep protocol/session code free of device globals and devicetree dependencies so native tests exercise the exact production logic.
- Use one physical IPC Service instance, one session owner, and endpoint names `imx8mp.mpipe.ctrl.v1` and `imx8mp.mpipe.audio.v1`.
- Every callback is bounded and nonblocking. Transmit-buffer acquisition is always `K_NO_WAIT`; after acquisition, failure is session-fatal because rollback is unavailable.
- Do not connect DMIC capture or inference in this plan.

---

## AMENDED 2026-09-12 — read this before any task below

Seven read-only audits, one hardware measurement and four build experiments
changed what this plan should build. The evidence is in
`docs/superpowers/specs/2026-09-11-imx8mp-m7-hifi4-ipc-design.md`; the
consequences for this plan are:

1. **Task 1 is DONE, in a much smaller form than written below.** The control
   header is 4 bytes, `{ cmd }` — a 16-bit type and a 16-bit transaction id —
   not the 40-byte header with CRC32C this plan describes. `ipc_service`
   already supplies the length and names endpoints, so a size and a stream id
   earn no place; the CRC goes because SOF carries none on a shared-DDR path
   and a checksum cannot detect the stale-cache-line failure this project has
   already hit. Credits are deleted in favour of per-stream
   underrun/overrun policy bits. The golden-vector generator described in
   Task 1 Step 1 was built, then retired once the header reached 4 bytes; it
   is recoverable at `a0275857da9` and the pattern belongs on the Linux
   supervisor's management protocol, where two languages actually meet.
   Delivered by `7303392bd34` and `4dfc48e40cb`.
2. **Task 2's retained-generation publish/claim/revalidate protocol is
   WITHDRAWN.** It was the most intricate mechanism in the design and served
   the least likely scenario. What replaces it is Zephyr ICMsg's session
   handshake, already implemented in `mpipe_ipc_session`: one 32-bit word per
   direction, request in the low half and acknowledgement in the high half,
   with a new value read back out of shared memory and incremented past both
   the reserved zero and the peer's last acknowledgement. The
   `mpipe_ipc_generation_io` indirection, `mpipe_ipc_host_publish`,
   `mpipe_ipc_remote_claim` and `mpipe_ipc_remote_revalidate` are not to be
   written.
3. **The eight-state session machine is not required for restart detection.**
   The latched session word is what makes detection work. Any state machine
   must be justified on its own terms.
4. **A rendezvous barrier is mandatory and is the remaining Task 2 work.**
   OpenAMP's host zeroes *both* vrings on every `open()` (`virtio.c:84`) and
   `virtio_reset_device()` has zero callers anywhere, so there is no
   reconciliation path: a restarted host wipes a live peer's receive ring
   before any stamped message can be exchanged. The survivor must not touch a
   ring, and must not call `ipc_service_open_instance()`, until the peer has
   published a session and finished initialising.
5. **Backend settled: `ipc_rpmsg_static_vrings`.** Both candidates build on
   both cores and their op tables are identical; static-vrings wins on vendor
   neutrality, Xtensa coverage, cache-line assertions and having no heap.
   Task 4 should not re-open this.
6. **Shared memory is non-cacheable with no cache maintenance**, matching NXP
   on both the M7 and Linux sides. Zephyr's alignment assertions only check
   the local core's line size, and the M7 and HiFi4 disagree.
7. **Declare your own MU node.** The in-tree i.MX8M `mailbox0` nodes are
   `nxp,imx-mu` (the old IPM binding, no `#mbox-cells`), not the MBOX binding
   `ipc_service` needs. Affects Task 3.
8. **Bulk audio leaves the message path**, confirmed by three production
   stacks. Task 6 carries PCM on a shared ring with two 32-bit offsets, not
   through the vring.

Where the task text below contradicts this section, this section wins.

### Task 1: Implement the canonical wire codec test-first

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_protocol.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_protocol.c`
- Create: `subsys/mpipe/ipc/Kconfig`
- Create: `subsys/mpipe/ipc/CMakeLists.txt`
- Modify: `subsys/mpipe/Kconfig`
- Modify: `subsys/mpipe/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_protocol/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_protocol/prj.conf`
- Create: `tests/subsys/mpipe/ipc_protocol/tests.yaml`
- Create: `tests/subsys/mpipe/ipc_protocol/vectors/mpipe-ipc-v1.json`
- Create: `tests/subsys/mpipe/ipc_protocol/scripts/vector_to_header.py`
- Create: `tests/subsys/mpipe/ipc_protocol/scripts/test_vector_to_header.py`
- Create: `tests/subsys/mpipe/ipc_protocol/src/main.c`

**Interfaces:**

```c
int mpipe_ipc_encode(void *dst, size_t capacity,
		     const struct mpipe_ipc_message *message, size_t *written);
int mpipe_ipc_decode(struct mpipe_ipc_message *message,
		     const void *src, size_t length);
int mpipe_ipc_validate_audio(const struct mpipe_ipc_message *message,
			     const struct mpipe_ipc_audio_format *expected);
```

The public structures use host-endian fields and spans into caller-owned
storage; packed C structs are never cast over wire bytes.

- [ ] **Step 1: Add failing golden-vector and rejection tests**

Make `vectors/mpipe-ipc-v1.json` the checked-in cross-language golden source.
Cover the 40-byte common header, every fixed control body, the 16-byte audio
descriptor, a complete 696-byte audio frame, and CRC32C check value
`0xe3069283`. The host script validates the JSON schema and emits a C header in
the build directory containing explicit byte arrays, so C and later Python
tests consume identical bytes and catch host endianness/padding. Add
table-driven failures for short input, arithmetic overflow, payload/header
mismatch, zero session, wrong stream/type, unknown mandatory flags, nonzero
reserved bytes, bad result/transaction rules, unsupported version/format, and
corrupt CRC.

```c
ZTEST(mpipe_ipc_protocol, test_header_golden_bytes)
{
	uint8_t encoded[40];
	const uint8_t expected[40] = {
		'M', 'I', 'P', 'C', 1, 0, MPIPE_IPC_HEARTBEAT, 40,
		/* remaining explicit little-endian fields and golden CRC */
	};
	/* Build a known host-endian message, encode, and compare all 40 bytes. */
	zassert_mem_equal(encoded, expected, sizeof(expected));
}
```

- [ ] **Step 2: Run the tests and confirm red for missing APIs**

```bash
python3 -m unittest discover -v \
  -s tests/subsys/mpipe/ipc_protocol/scripts -p 'test_*.py'
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_protocol \
  -p native_sim/native/64 -p qemu_cortex_m3
```

Expected: build failures identify only the not-yet-created codec interfaces.
The Python generator test must already pass; it verifies deterministic ordering,
required fields, even hex length, declared lengths, and refusal to overwrite a
source-tree file.

- [ ] **Step 3: Implement explicit little-endian encode/decode and CRC**

Use `sys_put_le16/sys_put_le32` and `sys_get_le16/sys_get_le32`. Encoding zeros
the CRC field in the destination and calls
`crc32_c(0, dst, 40 + payload_length, true, true)`. Decoding copies only the
40-byte header, zeros its CRC field, calls `crc32_c(0, header, 40, true,
payload_length == 0)`, then—when nonempty—continues with
`crc32_c(crc, payload, payload_length, false, true)`. Check all bounds before
addition/copy and expose payload only after complete structural and CRC
validation. Define all type, flag, result, state, format, role, and size
constants in the public header with `BUILD_ASSERT` coverage for v1 sizes.

- [ ] **Step 4: Prove portability and malformed-input behavior**

```bash
python3 -m unittest discover -v \
  -s tests/subsys/mpipe/ipc_protocol/scripts -p 'test_*.py'
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_protocol \
  -p native_sim/native/64 -p qemu_cortex_m3 --inline-logs
git diff --check
```

Expected: both platforms pass; unaligned input tests pass; no direct cast of a
wire buffer to a protocol structure appears in the diff.

- [ ] **Step 5: Commit the codec layer**

```bash
git add include/zephyr/mpipe/ipc subsys/mpipe tests/subsys/mpipe/ipc_protocol
git commit -s -m "subsys: mpipe: add versioned IPC wire codec"
```

### Task 2: Implement retained-generation and session state machines

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_session.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_session.c`
- Create: `tests/subsys/mpipe/ipc_session/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_session/prj.conf`
- Create: `tests/subsys/mpipe/ipc_session/tests.yaml`
- Create: `tests/subsys/mpipe/ipc_session/src/main.c`

**Interfaces:**

```c
struct mpipe_ipc_generation_io {
	uint32_t (*load_le32)(const volatile void *address);
	void (*store_le32)(volatile void *address, uint32_t value);
	void (*acquire)(void);
	void (*release)(void);
};

int mpipe_ipc_host_publish(volatile struct mpipe_ipc_generation *block,
			   const struct mpipe_ipc_generation_io *io,
			   uint32_t *generation);
int mpipe_ipc_remote_claim(volatile struct mpipe_ipc_generation *block,
			   const struct mpipe_ipc_generation_io *io,
			   uint32_t previous, uint32_t *claimed);
int mpipe_ipc_remote_revalidate(volatile struct mpipe_ipc_generation *block,
				const struct mpipe_ipc_generation_io *io,
				uint32_t claimed);
int mpipe_ipc_session_receive(struct mpipe_ipc_session *session,
			      const struct mpipe_ipc_message *message,
			      struct mpipe_ipc_action *action);
```

- [ ] **Step 1: Write failing generation-interleaving and state tests**

Use an instrumented fake retained block that can interleave a host reset after
each DSP load/store. Cover invalid magic/version/length, generation zero,
inverse mismatch, wrap to one, DSP-first wait, M7-first claim, torn DSP claim,
host reset before and after claim, revalidation immediately before open, DSP
reboot before open, DSP reboot after backend access, and one-core restart.

State tests cover BOOT through FAULT, one outstanding transaction per direction,
same-ID retry once after one second, wrong ACK ID, control/audio sequence wrap,
duplicate/stale/gap counters, three missed heartbeats, and eight malformed
messages inside one second. Assert one-core restart always produces FAULT and
never an in-place rebind.

- [ ] **Step 2: Confirm the new suite fails before implementation**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_session -p native_sim/native/64
```

- [ ] **Step 3: Implement generation ordering and deterministic state actions**

Make the state machine pure: it returns actions such as SEND, DROP, RELEASE,
RETURN_CREDIT, QUIESCE, or FAULT and never calls IPC Service itself. Session ID
is nonzero and supplied by the platform entropy source; tests inject it. Host
publication occurs only after vring/status reset. Remote claim persists before
backend access and is re-read under an acquire barrier immediately before open.

- [ ] **Step 4: Run tests and commit the session layer**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_session -p native_sim/native/64
git diff --check
git add include/zephyr/mpipe/ipc subsys/mpipe/ipc tests/subsys/mpipe/ipc_session
git commit -s -m "subsys: mpipe: add paired IPC session state machine"
```

### Task 3: Add disabled MU3 SoC nodes and production sample overlays

**Files:**
- Modify: `dts/arm/nxp/imx/nxp_imx8ml_m7.dtsi`
- Modify: `dts/xtensa/nxp/nxp_imx8m.dtsi`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/CMakeLists.txt`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/Kconfig`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/prj.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/main.c`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/dsp_fw_ready.c`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_m7.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_m7.overlay`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_adsp.conf`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_adsp.overlay`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_m7_hifi4_shared.h`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/README.rst`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/tests.yaml`

**Interfaces:** M7 `mu3_a` is `0x30e80000/IRQ138`; DSP `mu3_b` is
`0x30e90000/IRQ7`. Both nodes default disabled. Overlays enable one physical
`zephyr,ipc-openamp-static-vrings` instance using the backend region
`0xa0000000-0xa003ff7f`; the retained block starts at `0xa003ff80`.

- [ ] **Step 1: Add build assertions and negative configuration cases**

The shared header defines base, enclosing size, backend size, generation base,
alignment, buffer size, and descriptor count once, then asserts:

```c
BUILD_ASSERT(MPIPE_IPC_BACKEND_SIZE == 0x0003ff80U);
BUILD_ASSERT(MPIPE_IPC_GENERATION_BASE == 0xa003ff80U);
BUILD_ASSERT(MPIPE_IPC_GENERATION_BASE + 128U == 0xa0040000U);
BUILD_ASSERT(MPIPE_IPC_SHMEM_REQUIRED(64U, 1024U, 128U) <=
	     MPIPE_IPC_BACKEND_SIZE);
BUILD_ASSERT(MPIPE_IPC_SHMEM_REQUIRED(128U, 1024U, 128U) >
	     MPIPE_IPC_BACKEND_SIZE);
```

Use a sizing macro equivalent to the backend's `vq_ring_size`, `vring_size`,
and status-alignment calculation. Add a native test showing the backend's
power-of-two selection is exactly 64 descriptors. The second assertion guards
the proven 269,568-byte 128-descriptor overflow rather than relying on
documentation.

- [ ] **Step 2: Add complementary devicetree mappings**

M7 uses `mboxes = <&mu3_a 0>, <&mu3_a 1>` and names `tx`, `rx` with role
`host`; DSP uses `<&mu3_b 1>, <&mu3_b 0>` with names `tx`, `rx` and role
`remote`. Both set 128-byte alignment and 1024-byte buffer size through the
binding-supported properties/configuration. The DSP overlay creates no MU2
mailbox node. M7 host enables `CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET`;
DSP does not.

The first `main.c` is a build-only topology checker: it validates the enabled
IPC device and shared-address constants, then exits without opening the backend
or sending a mailbox. `dsp_fw_ready.c` is compiled but not invoked until the
deterministic runtime task. This keeps the source buildable without claiming a
hardware result.

- [ ] **Step 3: Build both overlays and inspect generated DTS/config/map**

```bash
transport_m7_build=$(mktemp -d /tmp/imx8mp-transport-m7.XXXXXX)
transport_dsp_build=$(mktemp -d /tmp/imx8mp-transport-dsp.XXXXXX)
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc build -p always \
  -d "$transport_m7_build" -b imx8mp_evk/mimx8ml8/m7 \
  samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc build -p always \
  -d "$transport_dsp_build" -b imx8mp_evk/mimx8ml8/adsp \
  samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4
```

Inspect both `zephyr.dts`, `.config`, linker maps, and ELF program headers.
Require exact addresses/channels/roles, 64 buffers, no generation overlap, and
exactly one enabled DSP mailbox interrupt device.

- [ ] **Step 4: Commit DT and configuration separately**

```bash
git add dts samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4
git commit -s -m "boards: nxp: add i.MX8MP direct MU3 IPC topology"
```

### Task 4: Implement the one-owner IPC Service adapter

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_transport.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_transport.c`
- Create: `tests/subsys/mpipe/ipc_transport/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_transport/prj.conf`
- Create: `tests/subsys/mpipe/ipc_transport/tests.yaml`
- Create: `tests/subsys/mpipe/ipc_transport/src/fake_ipc_service.c`
- Create: `tests/subsys/mpipe/ipc_transport/src/main.c`

**Interfaces:**

```c
struct mpipe_ipc_ops {
	int (*register_endpoint)(const struct device *, struct ipc_ept *,
				 const struct ipc_ept_cfg *);
	int (*get_tx_buffer)(struct ipc_ept *, void **, uint32_t *, k_timeout_t);
	int (*send_nocopy)(struct ipc_ept *, const void *, size_t);
	int (*hold_rx_buffer)(struct ipc_ept *, void *);
	int (*release_rx_buffer)(struct ipc_ept *, void *);
};

int mpipe_ipc_transport_init(struct mpipe_ipc_transport *transport,
			     const struct mpipe_ipc_transport_config *config);
int mpipe_ipc_transport_start(struct mpipe_ipc_transport *transport);
int mpipe_ipc_transport_send(struct mpipe_ipc_transport *transport,
			     enum mpipe_ipc_endpoint endpoint,
			     const struct mpipe_ipc_message *message);
int mpipe_ipc_transport_quiesce(struct mpipe_ipc_transport *transport);
```

- [ ] **Step 1: Write failing fake-backend ownership tests**

Assert both endpoints bind before HELLO, physical close happens once, a control
endpoint error faults audio too, all gets use `K_NO_WAIT`, no-credit avoids get,
no-buffer drops newest, encoding completes before get, post-get send failure is
fatal, RX callback holds then queues only pointer/length, queue-full releases
exactly once, worker release returns exactly one credit, and STOP_ACK is withheld
until held buffers and credits balance.

- [ ] **Step 2: Run red tests, implement minimal adapter, then rerun**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_transport -p native_sim/native/64
```

Callbacks may submit fixed-size work or
`k_msgq_put(&transport->rx_queue, &item, K_NO_WAIT)` only.
Endpoint callbacks receive the parent transport through `priv`; no singleton or
`DEVICE_DT_GET` appears in reusable transport code. The transport owns its two
`ipc_ept` objects and the single session/credit ledger.

- [ ] **Step 3: Add saturation, timing, and fault-window tests**

Run 100,000 fake frames while withholding releases in deterministic bursts.
Verify exact no-credit, no-buffer, queue-full, CRC, duplicate, gap, and stale
counters; zero leaks; no credit total above eight; and callback work bounded to
validation, hold, queue, and return. Inject the eighth malformed message inside
one second and confirm FAULT; confirm a clean second resets the rolling count.

- [ ] **Step 4: Commit the adapter**

```bash
git diff --check
git add include/zephyr/mpipe/ipc subsys/mpipe/ipc tests/subsys/mpipe/ipc_transport
git commit -s -m "subsys: mpipe: add coordinated IPC transport"
```

### Task 5: Add the M7 MU1 Linux-management bridge

**Files:**
- Create: `include/zephyr/mpipe/ipc/mpipe_ipc_management.h`
- Create: `subsys/mpipe/ipc/mpipe_ipc_management.c`
- Modify: `subsys/mpipe/ipc/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_management/CMakeLists.txt`
- Create: `tests/subsys/mpipe/ipc_management/prj.conf`
- Create: `tests/subsys/mpipe/ipc_management/tests.yaml`
- Create: `tests/subsys/mpipe/ipc_management/src/main.c`
- Create: `tests/subsys/mpipe/ipc_management/src/fake_rpmsg.c`
- Create: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/m7_linux_management.c`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/CMakeLists.txt`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/main.c`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_m7.conf`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/boards/imx8mp_evk_mimx8ml8_m7.overlay`

**Interfaces:** The M7 is an OpenAMP resource-table remote on its existing MU1
Linux link and advertises `imx8mp.mpipe.mgmt.v1`. The management bridge consumes
the same validated 40-byte codec but exposes only PAIR_QUERY, PAIR_STATE,
PAIR_START, PAIR_START_ACK, PAIR_QUIESCE, PAIR_QUIESCED, PAIR_FAULT, and
HEARTBEAT messages. It never carries AUDIO or direct-link CREDIT.

```c
struct mpipe_ipc_management_ops {
	int (*send)(void *context, const void *data, size_t length);
	int (*start_stream)(void *context, uint32_t transaction_id);
	int (*quiesce_stream)(void *context, uint32_t transaction_id);
};

int mpipe_ipc_management_init(struct mpipe_ipc_management *management,
			      const struct mpipe_ipc_management_config *config);
int mpipe_ipc_management_receive(struct mpipe_ipc_management *management,
				 const void *data, size_t length);
void mpipe_ipc_management_report_fault(struct mpipe_ipc_management *management,
				       const struct mpipe_ipc_error *error);
```

- [ ] **Step 1: Write failing management-policy tests**

Use fake send/direct-session callbacks. Cover query in every state, PAUSED only
after direct HELLO/CONFIG, nonzero transaction enforcement, same-ID duplicate
START returning the cached ACK without restarting, different-ID START while
streaming returning current state, QUIESCE stopping new capture before direct
STOP, QUIESCED only after balanced direct credits/holds, wrong stream/type
rejection, 500 ms heartbeat replies, bounded PAIR_FAULT, stale Linux session,
and queue-full/fault behavior. Explicitly assert AUDIO and CREDIT are rejected
on MU1.

- [ ] **Step 2: Run red tests and implement the pure bridge**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_management -p native_sim/native/64
```

The OpenAMP receive callback validates length, copies at most one bounded
management datagram into a fixed-depth queue with `K_NO_WAIT`, and returns. A
worker runs policy and sends ACK/status. It has injected operations and no
devicetree or static singleton, allowing exact native tests.

- [ ] **Step 3: Add M7-only resource-table platform glue**

Use the current `samples/subsys/ipc/openamp_rsc_table` implementation and NXP
resource-table definitions as the reviewed upstream pattern for MU1. Retain the
existing M7 `mailbox0`, `zephyr,ipc_shm` at its Linux management carveouts, and
Linux-owned resource table. Create one OpenAMP endpoint named exactly
`imx8mp.mpipe.mgmt.v1`; do not add this vdev or endpoint to the DSP firmware.
Keep MU1/OpenAMP teardown independent from the single MU3 IPC Service owner.

- [ ] **Step 4: Test and inspect the combined M7 build**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_management -p native_sim/native/64
ZEPHYR_SDK_INSTALL_DIR=/home/mt/zephyr-sdk-1.0.1 \
  /home/mt/zephyrproject/.venv/bin/west -z /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc build -p always \
  -d build/transport-m7-management -b imx8mp_evk/mimx8ml8/m7 \
  samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4
```

Inspect the resource table, MU1 management carveouts, MU1 and MU3 mailbox
instances, linker map, endpoint strings, and ELF segments. Require one MU1
Linux vdev, one separate MU3 static-vrings instance, and no overlap with the
direct reservation.

- [ ] **Step 5: Commit the M7 management bridge separately**

```bash
git diff --check
git add include/zephyr/mpipe/ipc subsys/mpipe/ipc \
  tests/subsys/mpipe/ipc_management \
  samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4
git commit -s -m "subsys: mpipe: add M7 pair management bridge"
```

### Task 6: Prove the deterministic two-core transport

**Files:**
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/CMakeLists.txt`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/Kconfig`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/prj.conf`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/main.c`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/dsp_fw_ready.c`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/src/m7_linux_management.c`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/README.rst`
- Modify: `samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4/tests.yaml`

**Interfaces:** M7 emits deterministic 640-byte PCM-shaped payloads. DSP
validates them, holds/releases buffers on a worker, and reports counters over
the direct control endpoint. The M7 also reports pair state/faults through MU1
management, but automatic test mode may initiate direct START until the Linux
production supervisor exists. There is no microphone or model dependency.

- [ ] **Step 1: Implement build-only deterministic roles**

DSP performs the approved one-shot MU2 READY before generation wait. M7 resets
backend state, publishes generation, and opens as host. The pair completes
HELLO, CONFIG, START, 10,000 frames, pressure bursts, STOP drain, and balanced
credit/hold assertions. M7 management emits matching PAUSED, STREAMING,
QUIESCED, and FAULT state reports; automatic test mode does not accept arbitrary
Linux commands. Banners include source/HAL IDs and all counters.

- [ ] **Step 2: Run all native tests and both target builds fresh**

```bash
env -u ZEPHYR_BASE /home/mt/zephyrproject/.venv/bin/python /home/mt/zephyrproject/worktrees/imx8mp-m7-hifi4-ipc/scripts/twister \
  -T tests/subsys/mpipe/ipc_protocol \
  -T tests/subsys/mpipe/ipc_session \
  -T tests/subsys/mpipe/ipc_transport \
  -T tests/subsys/mpipe/ipc_management \
  -p native_sim/native/64
```

Repeat the two board builds from Task 3 into newly created temporary build
directories and record SHA-256, DTS/config excerpts, ELF segments, and maps.

- [ ] **Step 3: Present a new board-run packet and wait**

Approval of the clean probe does not authorize these binaries. Present exact
commits, diffs, hashes, start/stop commands, rollback, and expected counters.
Wait for explicit approval before deployment or a remoteproc write.

- [ ] **Step 4: Execute the approved transport matrix**

Test production DSP-first and alternate M7-first starts, 100,000 frames,
credit starvation, RX queue saturation, STOP drain, paired restart, retained
memory across restart, DSP reboot before open, DSP reboot after access, M7-only
restart, DSP-only restart, and injected CRC/control timeout. One-core cases must
fault and require paired recovery; no case may resume stale streaming.

- [ ] **Step 5: Record the transport gate and commit documentation**

Classify results as source, build-only, or hardware. Record firmware/DTB hashes,
kernel identity, start order, generations, session IDs, and final counters. A
leak, stale resume, unexplained interrupt, PM/clock regression, or rollback
failure blocks the audio plan.

```bash
git diff --check
git add samples/subsys/ipc/ipc_service/imx8mp_m7_hifi4 docs
git commit -s -m "samples: ipc: prove i.MX8MP M7-HiFi4 transport"
```
