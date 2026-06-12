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
```

Do not add empty directories. Create each directory only when the first real
program lands.

## Build And CTest Policy

Add these switches when the first program lands:

- `RDMA_BUILD_STRESS_TESTS=OFF` by default.
- `RDMA_BUILD_PERFORMANCE_TESTS=OFF` by default.
- `RDMA_ENABLE_HARDWARE_TESTS=OFF` remains the outer hardware gate.
- `RDMA_TEST_ADDR=<ip>` remains the local RDMA-capable address.
- `RDMA_TEST_PEER_ADDR=<ip>` optional peer address for two-host tests.
- `RDMA_TEST_BASE_PORT=<port>` remains the port allocator base.

CTest labels:

- `stress`: long-running stability/concurrency test.
- `performance`: throughput-oriented benchmark.
- `latency`: latency-oriented benchmark.
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

## Perftest Comparison Strategy

Use perftest as a baseline, not as a pass/fail oracle.

Comparison rules:

- Compare IBV backend first, because perftest is verbs-oriented.
- Keep RDMA-on-Asio and perftest on the same hosts, NIC ports, GID/SGID choice,
  MTU, message size, queue depth, inline threshold, CQ moderation, and CPU
  affinity where possible.
- Compare like with like:
  - send/recv bandwidth against send bandwidth tools;
  - send/recv latency against send latency tools;
  - RDMA read/write bandwidth against read/write bandwidth tools;
  - RDMA read/write latency against read/write latency tools.
- Record command lines and environment alongside results.
- Treat ND backend results as RDMA-on-Asio internal measurements unless an
  equivalent NetworkDirect baseline tool is selected later.

Expected output from RDMA-on-Asio tools should be machine-readable:

```text
backend=ibv
mode=poll
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
errors=0
```

Also print a compact human-readable summary for local runs.

## Measurement Controls

Every performance/latency executable should accept command-line options rather
than depending on CMake-generated values:

- `--local-addr`
- `--peer-addr`
- `--port`
- `--server` / `--client`
- `--operation=send|recv|read|write|send_recv`
- `--mode=event|poll`
- `--message-size`
- `--iterations`
- `--queue-depth`
- `--qps`
- `--threads`
- `--warmup-iterations`
- `--inline-size`
- `--json-out`

CTest can pass defaults from cache variables, but the source should remain
build-system neutral.

## Stage 1 -- Minimal Benchmark Harness

- Add a small command-line parser for benchmark tools.
- Add a result struct and text/JSON output helper.
- Add a steady-clock timing helper.
- Add a two-process client/server handshake convention:
  - server listens and prints ready;
  - client connects and drives the measurement;
  - both sides agree on operation, message size, queue depth, and iterations.
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

- Match message sizes across a sweep such as 1, 2, 4, 8, 16, 32, 64, 128, 256,
  512, 1024, 2048, 4096, 8192, 16384, and 65536 bytes.
- Match queue depth for bandwidth tests.
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

- Compare read and write separately.
- Keep queue depth and message size aligned.
- For latency, use one outstanding operation unless explicitly measuring
  pipelined latency.

## Stage 4 -- Concurrency Stress

Add stress tests for:

- Multiple QPs sharing the event-mode completion service.
- Several `io_context::run()` threads.
- Several submitter threads posting send/recv or read/write.
- Poll-mode QPs sharing user-driven poll loops where supported.

Success criteria:

- No crashes, hangs, leaked terminal state, or unexpected error codes.
- All submitted operations either complete successfully or with a documented
  cancellation/teardown code.
- Watchdog timeout is a failure, not a skip.

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

Do not make performance numbers a normal CI pass/fail threshold until a stable
dedicated hardware runner exists. In ordinary development, use thresholds only
for sanity checks such as "nonzero throughput", "all iterations completed", and
"p99 latency was recorded".

## Acceptance Criteria

- Unit and correctness integration tests remain separate from stress/performance.
- Stress/performance/latency tests are never registered in default CTest.
- Benchmark tools can run as explicit client/server programs.
- Results include enough metadata to compare against perftest runs.
- IBV backend has a clear perftest comparison path for send, read, and write.
- ND backend can still run internal stress/performance tools without pretending
  to be directly comparable to verbs perftest.
