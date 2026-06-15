# RDMA Stress, Performance, And Latency Plan

## Goals

Build a separate hardware-only suite for:

- Stress testing RDMA-on-Asio state machines under high concurrency.
- Measuring throughput and latency for send/recv and RDMA read/write.
- Comparing RDMA-on-Asio results against the official RDMA perftest tools where
  the transport, device, queue depth, MTU, message size, and CPU placement are
  comparable.

This plan is intentionally separate from `unit_test_plan.md`. Unit and
correctness integration tests should stay small, deterministic, and focused on
API contracts. Stress/performance runs are opt-in, hardware-dependent, and
environment-sensitive.

## Scope Split

### Correctness Regression Stays In `unit_test_plan.md`

These tests assert behavior and should pass/fail by exact correctness rules:

- RDMA read/write round trip.
- Zero-length send/recv behavior.
- Multi-message ordering.
- Negative connect behavior.
- Existing private-data, SGL, cancellation, and disconnect integration tests.

### This Plan Owns Stress And Measurement

These programs produce stability or measurement signals:

- Multi-thread shared-CQ stress.
- Multiple QPs sharing one event-mode completion service.
- Repeated connect/disconnect soak.
- Send/recv bandwidth and latency.
- RDMA read/write bandwidth and latency.
- Poll-mode versus event-mode comparison.
- RDMA-on-Asio versus perftest comparison.

## Library Capability Prerequisites

Several scenario knobs that perftest relies on for its tuned defaults are **not
implemented in the current RDMA-on-Asio API**. Running a benchmark that silently
falls back to the library default while perftest uses its tuned default produces
an unfair comparison that looks like abstraction cost but is really a
feature-parity gap. Before a knob appears in a scenario, confirm the library
supports it; otherwise gate the scenario as a capability skip (see Skip Policy)
or constrain perftest to match.

Current support matrix (verify against the code before relying on it; line
references are a starting point, not a contract):

| Knob / scenario field | perftest default | Current library state | Action |
|-----------------------|------------------|-----------------------|--------|
| `signaled_every` / unsignaled WR + `cq_mod` | unsignaled, CQE every `--cq-mod` | **All sends are `IBV_SEND_SIGNALED`** (`ibv_ops_verbs.hpp`); one CQE per WR | Either add selective-signaling to the API, or run perftest with `--cq-mod 1` (signal every WR) for parity. Record which. |
| `inline_size` / `IBV_SEND_INLINE` | inline for small messages | **Inline forced off**: `attr.cap.max_inline_data = 0` (`ibv_service_verbs.hpp`); WR never sets `IBV_SEND_INLINE`, even though `rdma_config_t::max_inline_data_` exists | Either wire inline through, or run perftest with `--inline_size 0`. |
| `post_list` / `recv_post_list` (multi-WR batch per post) | batches N WRs per `ibv_post_send` | `async_send` posts **one** WR (with an SGL); no multi-WR batching | Map `post_list=1` only; skip `post_list>1` as missing capability. SGL fan-out is a separate dimension from WR batching. |
| `retry_count` / `rnr_retry` | configurable | **Hard-coded to 7** (`ibv_op_connect.hpp`, `ibv_service_connector.hpp`) | Record as fixed at 7; do not expose as a scenario knob until configurable. |

Rules:

- A scenario must not enable a knob the library cannot honor and then report the
  result as comparable. Either add the capability first, gate it as a skip with
  the missing-capability name, or constrain perftest to the library's behavior
  and label the run as a parity-constrained comparison.
- The first honest IBV-vs-perftest bandwidth comparison should run perftest with
  `--cq-mod 1` and `--inline_size 0` so both sides do one signaled, non-inline WR
  per message. Document this constraint in the result metadata.
- When a missing capability is added to the library later, re-baseline rather
  than comparing across the API change.

## Directory Layout

Proposed future layout:

```text
tests/
  stress/
    rdma/
      shared_cq.cpp
      connect_disconnect_soak.cpp
      multi_qp.cpp
  performance/
    rdma/
      send_recv_bw.cpp
      read_write_bw.cpp
      poll_vs_event.cpp
  latency/
    rdma/
      send_recv_lat.cpp
      read_write_lat.cpp
  native/
    nd/
      nd_send_bw.cpp
      nd_send_lat.cpp
      nd_write_bw.cpp
      nd_write_lat.cpp
      nd_read_bw.cpp
      nd_read_lat.cpp
  benchmark/
    scenarios/
      smoke.json
      baseline.json
      sweep.json
    tools/
      run_scenario.cpp
      compare_results.cpp
```

Do not add empty directories. Create each directory only when the first real
program lands.

## Build And CTest Policy

Add these switches when the first program lands:

- `RDMA_BUILD_STRESS_TESTS=OFF` by default.
- `RDMA_BUILD_PERFORMANCE_TESTS=OFF` by default.
- `RDMA_BUILD_NATIVE_BASELINES=OFF` by default.
- `RDMA_ENABLE_PERFTEST_BASELINE=OFF` by default.
- `RDMA_BUILD_PERFTEST=OFF` by default.
- `RDMA_PERFTEST_MODE=system` when `RDMA_ENABLE_PERFTEST_BASELINE=ON`.
- `RDMA_PERFTEST_BIN_DIR=<path>` optional path for external perftest binaries.
- `RDMA_ENABLE_HARDWARE_TESTS=OFF` remains the outer hardware gate.
- `RDMA_TEST_ADDR=<ip>` remains the local RDMA-capable address.
- `RDMA_TEST_PEER_ADDR=<ip>` optional peer address for two-host tests.
- `RDMA_TEST_BASE_PORT=<port>` remains the port allocator base.

CTest labels:

- `stress`: long-running stability/concurrency test.
- `performance`: throughput-oriented benchmark.
- `latency`: latency-oriented benchmark.
- `baseline`: direct external or native backend baseline.
- `hardware`: requires RDMA hardware.
- `manual`: not part of default CI.
- `nd` / `ibv` / `rdma`: backend scope.

Default `ctest` must not run stress/performance/latency tests. Even when built,
these should be explicitly selected, for example:

```text
ctest --test-dir build -C Release -L "performance|latency"
ctest --test-dir build -C Release -L stress
```

Prefer Release builds for measurement. Debug builds are useful for stress
correctness and sanitizer-style diagnosis, but not for performance numbers.

## External Benchmark Dependency Policy

`linux-rdma/perftest` is the preferred verbs baseline for IBV performance
comparison. It should be available to the benchmark workflow, but it should not
become a dependency of the RDMA-on-Asio library itself.

Support three perftest modes:

- `system`: find already-installed perftest executables from `PATH`.
- `external`: use `--perftest-bin-dir=<path>` or CMake cache variable
  `RDMA_PERFTEST_BIN_DIR=<path>`.
- `managed`: if `RDMA_BUILD_PERFTEST=ON`, build or import a project-managed
  perftest checkout for benchmark use only.

Default policy:

- `RDMA_ENABLE_PERFTEST_BASELINE=OFF` by default.
- `RDMA_BUILD_PERFTEST=OFF` by default.
- `RDMA_PERFTEST_MODE=system` by default when perftest baseline is enabled.
- perftest targets are never linked into production library targets.
- Windows/NetworkDirect runs should probe perftest and normally report it as
  unavailable, then use the native ND baseline plan instead.

The benchmark runner can support either:

- `--perftest-bin-dir=<path>` for already-built perftest executables; or
- `--perftest-source-dir=<path>` for a locally cloned perftest repository, where
  the runner records the source commit and invokes the user's build output; or
- a CMake-managed perftest build when `RDMA_BUILD_PERFTEST=ON`.

This keeps the ownership boundary clear:

- RDMA-on-Asio benchmark tools measure this library.
- perftest tools measure the direct verbs baseline.
- comparison scripts normalize and compare the two result sets.

Every baseline run should record:

- perftest executable path;
- perftest git commit or package version;
- full command line;
- device, port, GID index, MTU, message size, queue depth, inline size, CQ
  moderation, and connection mode;
- host CPU model, NUMA placement, OS, kernel, driver, and firmware where
  available.

## Perftest Comparison Strategy

Use perftest as a baseline, not as a pass/fail oracle.

Primary operation mapping:

- RDMA-on-Asio send/recv bandwidth: compare with `ib_send_bw`.
- RDMA-on-Asio send/recv latency: compare with `ib_send_lat`.
- RDMA-on-Asio RDMA write bandwidth: compare with `ib_write_bw`.
- RDMA-on-Asio RDMA write latency: compare with `ib_write_lat`.
- RDMA-on-Asio RDMA read bandwidth: compare with `ib_read_bw`.
- RDMA-on-Asio RDMA read latency: compare with `ib_read_lat`.

First-class comparison rules:

- Compare IBV backend first, because perftest is verbs-oriented.
- **Constrain perftest to the library's current feature set for the first honest
  comparison.** The library posts one signaled, non-inline WR per message and
  does not batch WRs (see Library Capability Prerequisites). Run perftest with
  `--cq-mod 1` and `--inline_size 0` and without `--post_list` so both sides do
  the same work per message; otherwise perftest's tuned defaults make the gap
  look like abstraction cost when it is a feature-parity gap. Record the
  constraint in the result metadata.
- **Connection-setup parity.** The library's control plane is rdma_cm-based, so
  the closest perftest setup is `--rdma_cm` (`-R`). Connection setup is excluded
  from the measured window, so this matters mainly for the connect/setup
  benchmarks rather than steady-state throughput; note which path was used.
- Keep RDMA-on-Asio and perftest on the same hosts, NIC ports, GID/SGID choice,
  MTU, message size, queue depth, inline threshold, CQ moderation, and CPU
  affinity where possible.
- Use RC transport first. Add UC/UD only after the public API and backend
  semantics explicitly support those modes.
- Compare poll mode first. perftest's default polling path is the closest
  low-overhead verbs baseline. Event/completion-channel style runs should be a
  separate scenario and compared against perftest `--events`.
- Use the same message-size sweep for both tools.
- Use the same queue depth for bandwidth tests. For latency tests, default to
  one outstanding operation unless explicitly measuring pipelined latency.
- Keep server and client command-line options identical except for the peer
  address.
- Treat perftest latency as half round-trip one-way latency when comparing
  against RDMA-on-Asio request/response latency tools.
- Run both iteration-based and duration-based bandwidth modes:
  - iteration mode is easier to debug and reproduce;
  - duration mode is better for stable throughput comparison.
- Record command lines and environment alongside results.
- Treat ND backend results as RDMA-on-Asio internal measurements unless an
  equivalent NetworkDirect baseline tool is selected later. The current plan is
  to provide a native ND baseline tool in this repository instead of depending
  on perftest for Windows.

Expected output from RDMA-on-Asio tools should be machine-readable:

```text
backend=ibv
mode=poll
token_type=callback
operation=send_recv
message_size=4096
queue_depth=128
iterations=1000000
qps=1
threads=1
throughput_mbps=...
message_rate_mps=...
avg_latency_us=...
p50_latency_us=...
p99_latency_us=...
cpu_cycles_per_op=...
errors=0
```

Also print a compact human-readable summary for local runs.

## Measurement Semantics

Define the workload semantics before comparing numbers. Do not compare two
tools only because their names look similar.

Cross-cutting methodology (applies to every measured operation):

- **Completion token choice is part of the measurement.** In poll mode the data
  path must use a non-`io_context`-bound token (`callback` or `use_future`) so
  the completion fires inline on the polling thread; a `use_awaitable` /
  io_context-bound token posts the completion back to an io_context and measures
  post overhead instead of the data path. Record the token type with every run
  (see `--token-type`).
- **Latency sampling.** Per-percentile fields (p50/p90/p99) require either a
  full per-sample array (perftest's approach) or an HDR-style histogram. A
  running min/avg/max is not sufficient for percentiles. Decide and record which.
- **Clock overhead calibration.** At RDMA latency scales the timestamp read
  itself (`steady_clock` / RDTSC) is a non-trivial fraction of the interval.
  Measure the clock-read cost once at startup and record it alongside results;
  prefer a monotonic high-resolution source and pin the measuring thread.
- **Sender/receiver roles.** Name the direction explicitly: for send/recv the
  client is the sender and the server the receiver; bytes are counted at the
  sender. For RDMA read/write the client is the initiator.

Send/recv bandwidth:

- Pre-post receives before the measured window.
- **Continuously replenish receive WRs during the window.** If the receiver runs
  out of posted recvs the sender hits RNR retry (with `rnr_retry` fixed at 7),
  which silently inflates latency and depresses throughput. Treat any observed
  RNR/retry event as a measurement-invalidating condition and record it.
- Start timing only after connection setup, memory registration, and warmup.
- Count payload bytes submitted by the sender.
- Keep data validation outside the measured window.
- Report both bytes/second and messages/second.

Send/recv latency:

- Use a ping-pong request/response loop for the perftest comparison path.
- Report one-way latency as half round-trip when using ping-pong.
- Keep a separate optional metric for local submit-to-completion latency, but do
  not compare that number directly against perftest send latency.
- Use one outstanding operation by default.

RDMA write bandwidth:

- The initiator writes into a registered remote buffer.
- Count initiator payload bytes.
- Keep remote-side validation outside the measured window.
- Add signaled completion ratio as an explicit scenario field if unsignaled
  writes are introduced later.

RDMA write latency:

- Measure signaled write completion latency on the initiator by default.
- If remote visibility acknowledgment is added later, label it as a separate
  acknowledged-write latency scenario.

RDMA read bandwidth and latency:

- The initiator reads from a registered remote buffer.
- Count bytes returned to the initiator.
- Keep the remote buffer stable for the full measurement.

Connection setup, memory registration, private-data exchange, and teardown are
not part of the primary throughput or latency metric. They can be measured by
separate connection/setup benchmarks later.

## Perftest Command Templates

The runner should generate command lines from scenario files rather than baking
these commands into CTest. Examples below show the shape of the baseline runs.

Send bandwidth:

```text
# server
ib_send_bw --ib-dev <dev> --ib-port <port> --gid-index <gid> --mtu <mtu> \
  --connection RC --size <bytes> --duration <sec> --margin <sec> \
  --tx-depth <depth> --rx-depth <depth> --cq-mod <n> --inline_size <bytes> \
  --port <tcp_port>

# client
ib_send_bw <server_ip> --ib-dev <dev> --ib-port <port> --gid-index <gid> \
  --mtu <mtu> --connection RC --size <bytes> --duration <sec> \
  --margin <sec> --tx-depth <depth> --rx-depth <depth> --cq-mod <n> \
  --inline_size <bytes> --port <tcp_port>
```

Send latency:

```text
# server
ib_send_lat --ib-dev <dev> --ib-port <port> --gid-index <gid> --mtu <mtu> \
  --connection RC --size <bytes> --iters <count> --inline_size <bytes> \
  --port <tcp_port>

# client
ib_send_lat <server_ip> --ib-dev <dev> --ib-port <port> --gid-index <gid> \
  --mtu <mtu> --connection RC --size <bytes> --iters <count> \
  --inline_size <bytes> --port <tcp_port>
```

RDMA write/read follow the same shape with `ib_write_bw`, `ib_write_lat`,
`ib_read_bw`, and `ib_read_lat`.

Optional scenario dimensions:

- `--events` for event/completion-channel comparison.
- `--rdma_cm` when comparing RDMA CM connection setup instead of socket-based
  out-of-band setup (this is the path that matches the library's control plane).
- `-b` bidirectional bandwidth -- deferred until the RDMA-on-Asio tools have a
  symmetric bidirectional mode; keep it out of scenarios until then rather than
  enabling it on one side only.
- `--qp <count>` for multi-QP scaling.
- `--post_list <count>` and `--recv_post_list <count>` only when RDMA-on-Asio
  has equivalent batching controls.
- `--report-histogram` or `--report-unsorted` for latency-distribution
  analysis. Keep these as opt-in because they can change output size and runtime
  overhead.

The comparison parser should tolerate perftest version differences by keeping
raw stdout/stderr and extracting only a small stable set of fields at first:

- operation;
- message size;
- iterations or duration;
- peak and average bandwidth where present;
- message rate where present;
- min/median/max latency for latency tools.

## NetworkDirect Native Baseline

perftest is verbs-focused. For NetworkDirect, use a native Windows baseline that
calls ND2 APIs directly without the RDMA-on-Asio wrappers.

Current status:

- Initial native ND baseline tools are implemented under `tests/native/nd/`.
- `native_nd_send_recv_bench` bypasses the RDMA-on-Asio wrapper and uses direct
  ND2 calls for polling send/recv bandwidth and ping-pong latency.
- `native_nd_read_write_bench` bypasses the RDMA-on-Asio wrapper and uses direct
  ND2 calls for polling RDMA write/read bandwidth and latency.
- Older Release results in `rdma_stress_performance_results.md` should remain
  labeled by their `baseline` field; `baseline=rdma_on_asio` results are not
  direct ND baseline results.

Proposed future layout:

```text
tests/
  native/
    nd/
      nd_send_bw.cpp
      nd_send_lat.cpp
      nd_write_bw.cpp
      nd_write_lat.cpp
      nd_read_bw.cpp
      nd_read_lat.cpp
```

Native ND baseline rules:

- Share the same scenario JSON schema and result JSON schema as RDMA-on-Asio
  benchmark tools.
- Share the same build/runtime switches:
  - `RDMA_BUILD_NATIVE_BASELINES=OFF` by default;
  - `RDMA_ENABLE_HARDWARE_TESTS=OFF` remains the outer registration gate;
  - native ND targets are labeled `baseline;native_nd;nd;hardware;manual`.
- Use direct provider, adapter, protection-domain, completion-queue, queue-pair,
  connector, and listener calls.
- Reuse common benchmark utilities for option parsing, result schema, raw log
  archiving, watchdog timeout, and report generation.
- Keep buffer registration, queue depth, inline size, message size, and CPU
  placement aligned with the RDMA-on-Asio ND run.
- Keep the native ND direct baseline poll-only. RDMA-on-Asio event mode remains
  the event-scheduler measurement; native ND event/IOCP is intentionally out of
  scope for this baseline.
- Keep setup, memory registration, connection establishment, and teardown
  outside the measured window.
- Validate data outside the measured window.
- Explicitly mark `baseline=native_nd` and `backend=nd` in result JSON.
- Store output beside the RDMA-on-Asio run so comparison tooling can create
  side-by-side tables.

Direct ND implementation outline:

- Capability probe:
  - enumerate ND providers/adapters;
  - verify the selected local address maps to an adapter;
  - record max SGE, max inline data, queue depth limits, and max private data.
- Send/recv bandwidth:
  - create provider, adapter, PD, CQ, QP, connector/listener directly;
  - pre-post receives on the server;
  - drive the same message-size and queue-depth sweep as RDMA-on-Asio.
- Send/recv latency:
  - use the same ping-pong semantics as RDMA-on-Asio latency;
  - pre-post the next receive before echoing the current message to avoid
    RNR-style stalls.
- RDMA write/read:
  - register local and remote memory directly through ND;
  - exchange remote address/token with a small control message or private data;
  - measure write and read separately.
- Completion modes:
  - implement the simplest direct completion path first;
  - keep native direct completion poll-only so it remains a stable data-path
    baseline.

This gives three comparable tracks:

- IBV RDMA-on-Asio versus perftest verbs baseline.
- ND RDMA-on-Asio versus native ND direct baseline.
- IBV RDMA-on-Asio versus ND RDMA-on-Asio as a portability and abstraction-cost
  signal, not as a hardware-equivalent comparison unless the hardware, link, and
  host configuration are equivalent.

## Measurement Controls

Every performance/latency executable should accept command-line options rather
than depending on CMake-generated values:

- `--local-addr`
- `--peer-addr`
- `--port`
- `--server` / `--client`
- `--backend=ibv|nd`
- `--operation=send|recv|read|write|send_recv`
- `--mode=event|poll`
- `--token-type=callback|use_future|use_awaitable` (poll mode must use a
  non-io_context token; `use_awaitable` is the coroutine-overhead measurement)
- `--message-size`
- `--iterations`
- `--duration-sec`
- `--margin-sec`
- `--queue-depth`
- `--qps`
- `--threads`
- `--warmup-iterations`
- `--inline-size`
- `--cq-mod`
- `--post-list`
- `--recv-post-list`
- `--signaled-every`
- `--gid-index`
- `--ib-dev`
- `--ib-port`
- `--mtu`
- `--cpu-affinity`
- `--numa-node`
- `--baseline=rdma_on_asio|perftest|native_nd`
- `--topology`
- `--scenario`
- `--csv-out`
- `--json-out`

CTest can pass defaults from cache variables, but the source should remain
build-system neutral.

The command-line parser should reject unsupported combinations explicitly. For
example, `--gid-index` is IBV-specific, while native ND tools should either
ignore it with a clear warning in compatibility mode or reject it in strict mode.

## Scenario Matrix

Use scenario files to keep perftest, native ND, and RDMA-on-Asio runs aligned.
A scenario should describe intent, not executable-specific flags.

Example schema shape:

```json
{
  "name": "send_bw_rc_poll_4k_qd128",
  "backend": "ibv",
  "baseline": ["rdma_on_asio", "perftest"],
  "operation": "send_recv",
  "metric": "bandwidth",
  "completion_mode": "poll",
  "token_type": "callback",
  "connection": "RC",
  "message_size": 4096,
  "queue_depth": 128,
  "qps": 1,
  "threads": 1,
  "iterations": 0,
  "duration_sec": 30,
  "margin_sec": 3,
  "inline_size": 0,
  "cq_mod": 64,
  "post_list": 1,
  "recv_post_list": 1,
  "signaled_every": 1,
  "mtu": 4096,
  "gid_index": 0,
  "topology": "single_host_two_process",
  "cpu_affinity": {
    "server": "auto",
    "client": "auto"
  }
}
```

Initial scenario dimensions:

- operations: send/recv, RDMA write, RDMA read;
- metrics: bandwidth, latency;
- completion modes: poll first, event second;
- token types: `callback` first (lowest overhead), then `use_awaitable` to
  measure coroutine cost; the gap between them is the coroutine abstraction cost;
- message sizes: 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
  8192, 16384, and 65536 bytes;
- queue depths for bandwidth: 1, 8, 32, 128, and 512 where supported;
- queue depth for latency: 1 by default;
- QPs: 1 first, then 2, 4, 8, and 16 for scaling tests;
- threads: 1 first, then one submitter per QP for scaling tests.

Do not run the full matrix by default. Provide named suites:

- `smoke`: one operation, one size, one queue depth, one QP.
- `baseline`: representative sizes and one QP.
- `sweep`: full size sweep.
- `scale`: QP/thread scaling.
- `event`: event-mode comparison.

Scenario compatibility rules:

- A scenario can be skipped only when a required capability is missing, not
  because the executable silently ignored a parameter.
- Unsupported fields should produce a clear error in strict mode.
- Capability-based skips must be written into the result JSON with the missing
  capability name.
- If a perftest option has no RDMA-on-Asio equivalent yet, keep that option out
  of the scenario instead of enabling it only on one side.

## Single-Host Plan

Current hardware reality: start with one physical machine.

Single-host modes:

- Same-process loopback:
  - server and client endpoints run in one process;
  - useful for API overhead, correctness under load, and fast iteration;
  - not a line-rate benchmark.
- Two-process same-host loopback:
  - one server process and one client process on the same machine;
  - closer to perftest usage and catches process-boundary setup issues;
  - still not a substitute for a real wire-speed two-host run.
- Dual-port same-host, if the NIC and cabling support it:
  - bind server and client to different RDMA ports;
  - optionally use external loopback cabling or switch ports;
  - record the physical topology explicitly.

Single-host results should be labeled with one of the concrete topology ids:

- `single_host_same_process`;
- `single_host_two_process`;
- `single_host_dual_port`.

They are valid for regression tracking and abstraction-overhead exploration, but
should not be presented as final network throughput claims.

Single-host run order:

1. RDMA-on-Asio smoke run for send/recv latency and bandwidth.
2. RDMA-on-Asio read/write smoke run.
3. IBV perftest loopback where the device/driver supports it.
4. Native ND direct baseline on Windows.
5. Duration-based RDMA-on-Asio versus baseline comparison for selected sizes.

For one-machine development, prioritize repeatability over absolute numbers:

- pin server/client processes to stable CPU sets;
- avoid running both sides on the same physical core;
- record NUMA node and NIC locality;
- run a warmup before measuring;
- repeat each scenario at least three times and report min/median/max across
  runs.

## Multi-Host Plan

When more physical machines are available, promote the benchmark suite from
development signal to real transport comparison.

Two-host minimum topology:

```text
host A                         host B
server process                 client process
RDMA NIC port <--------------> RDMA NIC port
same MTU/GID/link mode         same MTU/GID/link mode
```

Two-host requirements:

- same or explicitly documented NIC generation and firmware;
- stable link speed and MTU;
- synchronized clocks for log correlation, though latency should not depend on
  cross-host wall-clock timestamps;
- CPU affinity and NUMA binding selected near the NIC on both hosts;
- firewall and routing configuration documented for control-plane ports;
- identical benchmark binary revision and build type.

Two-host run order:

1. Verify direct verbs baseline with perftest send/write/read smoke tests.
2. Verify RDMA-on-Asio IBV smoke tests with the same operation and size.
3. Run latency scenarios first, because they catch setup and completion-path
   overhead quickly.
4. Run bandwidth duration scenarios.
5. Repeat selected scenarios in event mode.
6. Archive raw logs, JSON output, perftest output, and environment metadata from
   both hosts.

More-than-two-host extensions:

- Pairwise matrix: run every host pair independently to detect cabling, switch,
  and device differences.
- One-server, many-client fan-in: stress accept/connect and shared completion
  infrastructure.
- Independent pair saturation: run multiple host pairs at once to measure switch
  or fabric contention.
- Cross-backend comparison: keep IBV and ND runs separate unless the hardware
  and OS differences are intentionally part of the experiment.

Multi-host results should be labeled with a topology id, for example
`topology=two_host_direct`, `topology=two_host_switch`, or
`topology=multi_pair_switch`.

## Runner And Process Orchestration

Add a small runner after the first manual benchmark tools exist. The runner
should orchestrate processes and collect logs; it should not hide the raw
commands.

Runner responsibilities:

- allocate or accept a control-plane port range;
- launch server and client processes for RDMA-on-Asio tools;
- optionally launch perftest server and client processes;
- wait for an explicit server-ready line before starting the client;
- enforce startup, measurement, and teardown timeouts;
- terminate both sides on failure and preserve partial logs;
- write a run manifest containing scenario, commands, process ids, exit codes,
  timestamps, and output paths.

For multi-host runs, the first version can be manual:

- print the exact server command to run on host A;
- print the exact client command to run on host B;
- collect result files from both hosts into one output directory.

Later, add optional remote execution support only if it can stay outside the
core benchmark binaries.

## Capability Discovery And Skip Policy

Add lightweight probe commands before running large suites:

- list available IBV devices, ports, link layers, MTU, active link speed, and
  GID table entries where supported;
- list available ND providers, adapters, addresses, and completion modes where
  supported;
- detect whether perftest executables exist and record their versions;
- detect whether same-host loopback and dual-port scenarios are supported;
- verify that the selected control-plane port range is free.

Skip policy:

- Missing hardware, missing perftest, unsupported topology, or unsupported
  completion mode should be reported as `skipped`.
- Connection failure after a supposedly valid capability probe is a failure, not
  a skip.
- Watchdog timeout is a failure.
- Parser failure for perftest output is a reporting failure, but raw output must
  still be preserved.

## Environment Stabilization Checklist

Performance runs should record and, where practical, control the environment:

- Release build and compiler flags.
- CPU affinity for server, client, and polling threads.
- NUMA node for buffers and threads.
- CPU frequency governor or Windows power plan.
- Interrupt affinity and adaptive interrupt settings where relevant.
- NIC MTU, link speed, driver, and firmware.
- Huge page or large page usage if introduced later.
- Background load summary.
- Number of repetitions per scenario.

Reporting should show per-run values and aggregate at least min, median, and max
across repetitions. Do not claim a regression from a single noisy run unless the
change is very large and reproduced.

## Result Schema And Units

Use versioned machine-readable results from the first implementation.

Required result fields:

- `schema_version`;
- `scenario_name`;
- `backend`;
- `baseline`;
- `topology`;
- `operation`;
- `metric`;
- `completion_mode`;
- `token_type` (`callback` / `use_future` / `use_awaitable`);
- `message_size_bytes`;
- `queue_depth`;
- `qps`;
- `threads`;
- `iterations`;
- `duration_sec`;
- `warmup_iterations`;
- `payload_bytes`;
- `posted_count`;
- `completed_count`;
- `throughput_mib_s`;
- `throughput_gbit_s`;
- `message_rate_s`;
- `latency_avg_us`;
- `latency_min_us`;
- `latency_p50_us`;
- `latency_p90_us`;
- `latency_p99_us`;
- `latency_max_us`;
- `latency_sample_method` (`full_array` / `histogram`);
- `clock_overhead_ns`;
- `cpu_cycles_per_op` (abstraction-cost signal; `null` if not measured);
- `cpu_util_percent`;
- `context_switches`;
- `rnr_retry_events`;
- `validation_passed` (`true` / `false` / `null` when not validated);
- `errors`;
- `first_error`;
- `exit_code`;
- `skip_reason`;
- `missing_capability` (`null` unless skipped for a capability gap);
- `command_line`;
- `environment`.

CPU-efficiency fields (`cpu_cycles_per_op`, `cpu_util_percent`,
`context_switches`) are the core of the abstraction-cost comparison: at line
rate two tools can both saturate the link while differing greatly in CPU cost,
so throughput alone cannot answer "what does the Asio + coroutine layer cost?".

Unit rules:

- Use bytes for sizes.
- Use MiB/s for binary byte throughput and Gbit/s for network-style bit rate.
- Use microseconds for latency.
- Store raw perftest output next to parsed fields.
- Keep missing values as `null`, not `0`, when a metric is not applicable.

## Stage 0 -- Capability Probe And Smoke Validation

Implement this before the larger benchmark harness.

- Add a hardware capability probe executable or runner command.
- Check that the selected backend has at least one usable device/address.
- Check that the selected topology is possible on the current machine.
- Check that required baseline executables are present:
  - perftest for IBV baseline runs;
  - native ND tools for ND baseline runs.
- Run one tiny send/recv smoke scenario before any long suite.
- Emit JSON for probe results so skipped scenarios are explainable.

## Stage 1 -- Minimal Benchmark Harness

- Add a small command-line parser for benchmark tools.
- Add a result struct and text/JSON output helper.
- Add a scenario JSON loader and validator.
- Add a steady-clock timing helper, including a startup clock-overhead
  calibration (record `clock_overhead_ns`) and a latency-sample collector
  (full array or HDR histogram) capable of emitting p50/p90/p99.
- Add a CPU-cost capture helper (cycles-per-op / CPU% / context switches) so the
  abstraction-cost fields can be populated; leave them `null` where the platform
  cannot supply them.
- Add environment metadata collection:
  - git commit;
  - build type;
  - compiler;
  - OS and kernel;
  - CPU model;
  - NIC name, driver, firmware, MTU, and link speed when discoverable.
- Add a two-process client/server handshake convention:
  - server listens and prints ready;
  - client connects and drives the measurement;
  - both sides agree on operation, message size, queue depth, and iterations.
- Add a perftest command generator that turns scenarios into command templates
  without linking to perftest.
- Add a raw-output archiver for benchmark stdout/stderr.
- Keep this harness independent of the Asio unit-test macros.

## Stage 2 -- Send/Recv Bandwidth And Latency

Add RDMA-on-Asio tools for:

- Poll-mode send/recv bandwidth.
- Event-mode send/recv bandwidth.
- Poll-mode send/recv latency.
- Event-mode send/recv latency.

Key metrics:

- throughput in MiB/s and Gbit/s;
- message rate;
- average latency;
- p50/p90/p99 latency for latency mode;
- CPU count and thread count used by the test;
- error count and first error code.

Perftest comparison:

- Use `ib_send_bw` for bandwidth and `ib_send_lat` for latency.
- Match message sizes across a sweep such as 1, 2, 4, 8, 16, 32, 64, 128, 256,
  512, 1024, 2048, 4096, 8192, 16384, and 65536 bytes.
- Match queue depth, `--tx-depth`, `--rx-depth`, inline size, CQ moderation,
  MTU, GID index, device, and port for bandwidth tests where possible.
- Compare poll mode against default perftest first.
- Compare event mode only against perftest `--events` runs.
- Use a single QP first, then add multi-QP runs.

## Stage 3 -- RDMA Read/Write Bandwidth And Latency

Add tools for:

- RDMA write bandwidth.
- RDMA write latency.
- RDMA read bandwidth.
- RDMA read latency.

Design notes:

- Register local and remote memory regions once per run.
- Exchange `rdma_remote_addr_t` through private data or an explicit control
  message.
- Keep remote memory hot and pinned for the full measurement.
- Separate setup time from measured operation time.
- Validate final buffer contents outside the measured window.

Perftest comparison:

- Use `ib_write_bw` and `ib_write_lat` for RDMA write.
- Use `ib_read_bw` and `ib_read_lat` for RDMA read.
- Compare read and write separately.
- Keep queue depth and message size aligned.
- For latency, use one outstanding operation unless explicitly measuring
  pipelined latency.
- For ND, compare against the Stage 7 native ND direct send/recv and read/write
  baseline tools.

## Stage 4 -- Concurrency Stress

Add stress tests for:

- Multiple QPs sharing the event-mode completion service.
- Several `io_context::run()` threads.
- Several submitter threads posting send/recv or read/write.
- Poll-mode QPs sharing user-driven poll loops where supported.

This stage is the concrete realization of the long-standing shared-CQ poller
TODO. The event-mode shared-CQ poller is documented as a single self-perpetuating
op, lock-free and thread-safe under concurrent multi-thread `run()` plus
concurrent submission. The stress test should exercise and assert that contract
directly:

- Several `run()` threads plus several submitter threads posting concurrently
  over multiple QPs that share one io_context's CQ.
- The poller is started exactly once, lazily, at the first event-mode
  `queue_pair::bind(io)` and re-arms itself thereafter; no second poller appears
  as QPs/threads are added.
- The total `completed_count` equals `posted_count` (minus documented
  cancellations) -- a consistent completion count is the main race signal.
- Because the started poller is outstanding work for the io_context's lifetime,
  `run()` does not return on idle; shutdown is driven by `io.stop()` and must
  exit cleanly with all threads joined.
- io_contexts that never bind an event-mode QP (poll-only / control-plane-only)
  must still observe "run() returns when idle".

Success criteria:

- No crashes, hangs, leaked terminal state, or unexpected error codes.
- All submitted operations either complete successfully or with a documented
  cancellation/teardown code.
- `completed_count` is consistent with `posted_count` across the whole run.
- Clean `io.stop()` exit with all `run()` threads joined; a watchdog timeout is
  a failure, not a skip.

These tests should report operation counts and error histograms, but their main
purpose is stability, not benchmark-grade numbers.

## Stage 5 -- Connect/Disconnect Soak

Add long-running soak tests for:

- Repeated connect/accept/disconnect.
- Cancellation racing with connect.
- Disconnect racing with establishment.
- Wait-disconnect re-arm behavior.

Run dimensions:

- iterations;
- concurrent connectors;
- event-mode and poll-mode data-plane setup where applicable;
- optional randomized small delays to widen race coverage.

Keep deterministic regression tests for specific bugs in `unit_test_plan.md` or
the integration suite. This soak is for broad race discovery.

## Stage 6 -- Reporting And Baseline Tracking

Create a simple results workflow:

- Store raw JSON results under a user-specified output directory.
- Include git commit, backend, build type, compiler, OS, CPU model, NIC name, and
  command line.
- Keep perftest command lines in the same run directory.
- Generate a comparison table from RDMA-on-Asio JSON and perftest output.
- Generate a comparison table from RDMA-on-Asio ND JSON and native ND baseline
  JSON.
- Preserve raw stdout/stderr for all benchmark processes.
- Add a small parser layer for known perftest output fields, but keep the raw
  output as the source of truth when parser support lags behind a perftest
  version.
- Mark every result with topology:
  - `single_host_same_process`;
  - `single_host_two_process`;
  - `single_host_dual_port`;
  - `two_host_direct`;
  - `two_host_switch`;
  - `multi_host`.

Do not make performance numbers a normal CI pass/fail threshold until a stable
dedicated hardware runner exists. In ordinary development, use thresholds only
for sanity checks such as "nonzero throughput", "all iterations completed", and
"p99 latency was recorded".

## Stage 7 -- Native ND Baseline

Add direct NetworkDirect benchmark tools after the RDMA-on-Asio benchmark
harness is stable.

Stage status:

- Initial polling direct-ND baseline implemented for send/recv, RDMA write, and
  RDMA read.
- Native ND direct baselines are intentionally poll-mode only; native
  IOCP/event-style completion is not part of this plan.

Implementation priorities:

- Share benchmark argument parsing, scenario loading, result output, and
  metadata capture with the RDMA-on-Asio benchmark tools.
- Keep direct ND code in `tests/native/nd/`, not in the production library.
- Implement `nd_send_bw.cpp` first.
- Implement `nd_send_lat.cpp` second, using the same ping-pong semantics as
  `rdma_send_recv_bench`.
- Implement `nd_write_bw.cpp` / `nd_write_lat.cpp` third. The first version is
  folded into `native_nd_read_write_bench`.
- Implement `nd_read_bw.cpp` / `nd_read_lat.cpp` fourth. The first version is
  folded into `native_nd_read_write_bench`.
- Provide the simplest direct completion path first. This is implemented with
  CQ polling.
- Do not add native IOCP/event-style completion to this benchmark baseline; use
  RDMA-on-Asio event-mode tests for event scheduler measurements.
- Match memory-registration lifetime and queue depth with RDMA-on-Asio ND.
- Register native ND CTest entries only when both `RDMA_BUILD_NATIVE_BASELINES`
  and `RDMA_ENABLE_HARDWARE_TESTS` are enabled.

First acceptance target:

- `rdma_send_recv_bench` Release single-host bandwidth/latency results and
  `native_nd_send_recv_bench` Release single-host bandwidth/latency results can
  be written into the same result directory.
- `rdma_read_write_bench` Release single-host write/read results and
  `native_nd_read_write_bench` Release single-host write/read results can be
  written into the same result directory.
- The report can show side-by-side rows:
  - `baseline=rdma_on_asio`;
  - `baseline=native_nd`.
- Both rows use the same topology, message size, queue depth, iterations,
  completion mode, and local address.

This stage answers the Windows-specific abstraction-cost question that perftest
cannot answer directly.

## Coding Status After Initial Implementation

Implemented targets:

- `rdma_benchmark_capability_probe`
  - probes RDMA-on-Asio device availability through the active backend;
  - detects perftest executables from `PATH` or `--perftest-bin-dir`;
  - reports RDMA-on-Asio, perftest, and native-ND baseline capabilities as JSON.
- `rdma_benchmark_perftest_commands`
  - generates perftest server/client command templates for send, write, and
    read bandwidth/latency;
  - applies the initial parity constraints from this plan: RDMA CM, `cq_mod=1`,
    and `inline_size=0` unless the scenario overrides the corresponding field.
- `rdma_benchmark_run_scenario`
  - loads a scenario JSON and emits explicit single-process/server/client
    command lines;
  - supports `--execute` for `single_host_same_process`, writing manifest,
    stdout, stderr, and benchmark result JSON into the output directory;
  - two-process and multi-host runs still intentionally keep explicit
    server/client commands.
- `rdma_benchmark_compare_results`
  - reads one or more result JSON files and prints a Markdown comparison table.
- `rdma_benchmark_parse_perftest`
  - converts a saved perftest stdout file into the common result JSON schema;
  - keeps the parser intentionally conservative and reports a parser error
    rather than inventing numbers when it cannot find a stable numeric row.
- `rdma_send_recv_bench`
  - event-mode send/recv bandwidth and ping-pong latency;
  - poll-mode send/recv bandwidth and ping-pong latency through standalone CQ +
    `as_tuple(use_future)`;
  - `use_awaitable` latency remains a capability skip in the benchmark tool;
  - records basic process CPU utilization where available.
- `rdma_read_write_bench`
  - event-mode RDMA write/read bandwidth and latency;
  - poll-mode RDMA write/read bandwidth and latency through standalone CQ +
    `as_tuple(use_future)`;
  - exchanges `rdma_remote_addr_t` through accept/connect private data;
  - validates RDMA read on the client and RDMA write on the server;
  - records basic process CPU utilization where available.
- `rdma_shared_cq_stress`
  - single-host multi-QP event-mode stress over one shared `io_context` CQ;
  - supports multiple `io_context::run()` threads and checks completion counts.
- `rdma_connect_disconnect_soak`
  - repeated single-host connect/accept/disconnect soak with configurable
    iterations and `run()` thread count.
- `native_nd_send_recv_bench` and `native_nd_read_write_bench`
  - compile and emit the common JSON schema with `baseline=native_nd`;
  - `native_nd_send_recv_bench` uses direct ND2 adapter/CQ/QP/connector/listener
    and memory-region APIs for polling send/recv bandwidth and ping-pong
    latency;
  - `native_nd_read_write_bench` uses direct ND2 memory-region remote tokens
    exchanged through an explicit post-connect Send/Receive control message,
    then posts polling
    `IND2QueuePair::Write` and `IND2QueuePair::Read` requests;
  - native ND write validates the remote buffer on the server after disconnect;
  - native ND read validates the fetched buffer on the client;
  - native ND direct baselines are currently poll-mode only.

Current coding gaps that remain explicit capability skips or follow-up work:

- full two-process process orchestration with server-ready log parsing;
- deeper perftest parser coverage for all version-specific output formats;
- CPU efficiency fields beyond process CPU utilization (`cpu_cycles_per_op`,
  context switches);
- deep environment capture for NIC driver/firmware/MTU/link speed; the current
  result JSON records backend build, build type, compiler, platform, and C++
  standard.

## Acceptance Criteria

- Unit and correctness integration tests remain separate from stress/performance.
- Stress/performance/latency tests are never registered in default CTest.
- Benchmark tools can run as explicit client/server programs.
- Scenario files can drive RDMA-on-Asio, perftest, and native ND runs without
  requiring source-code changes.
- Measurement semantics are defined for send/recv, RDMA write, and RDMA read
  before performance numbers are compared.
- Scenario knobs are validated against actual library capability: features the
  API does not support (selective signaling, inline data, WR batching) are
  either implemented first, gated as capability skips, or matched by constraining
  perftest -- never compared with mismatched per-message work.
- The completion-token type (callback vs coroutine) is recorded, and poll-mode
  runs use a non-io_context token so the data path is what is measured.
- CPU-efficiency metrics are captured so abstraction cost can be assessed at line
  rate, not just throughput and latency.
- The Stage 4 stress test asserts the documented shared-CQ poller contract:
  single lazy poller, consistent completion count, and clean `io.stop()` exit.
- Capability probes distinguish unsupported scenarios from real failures.
- Results include enough metadata to compare against perftest runs.
- Result JSON is schema-versioned and uses explicit units.
- Runner timeouts and raw log preservation make hangs diagnosable.
- IBV backend has a clear perftest comparison path for send, read, and write.
- perftest remains an external executable baseline, not a vendored or linked
  dependency.
- ND backend has a clear native direct-ND comparison path instead of pretending
  to be directly comparable to verbs perftest.
- The plan explicitly distinguishes RDMA-on-Asio ND benchmark results from
  direct NetworkDirect baseline results.
- Single-host results are explicitly labeled as development/regression signals.
- Multi-host results have topology, host, NIC, and command-line metadata
  sufficient for later comparison.
