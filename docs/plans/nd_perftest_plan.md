# ND Perftest Plan

`nd_perftest` is the Windows NetworkDirect native benchmark that aligns with
`asio_perftest`, not with the full Linux `linux-rdma/perftest` feature surface.
Its job is to answer one question cleanly:

> On the same Windows host and ND adapter, how much cost does the RDMA-on-Asio ND
> backend add compared with direct ND2 API calls?

That makes `nd_perftest` the ND analogue of the IBV `asio_perftest` vs
`perftest` comparison, but the alignment target is the RDMA-on-Asio benchmark
harness and measurement semantics.

## Scope

- Compare `baseline=rdma_on_asio` / `backend=nd` against
  `baseline=native_nd` / `backend=nd`.
- Reuse the same scenario schema, CLI vocabulary, result JSON, stdout summary,
  raw-log layout, warmup/timed-window semantics, latency statistics, bandwidth
  peak/average rules, and CPU metrics as `asio_perftest`.
- Keep the native data path direct ND2 only: provider/adapter/PD/CQ/QP,
  connector/listener, MR registration, and `IND2QueuePair::{Send,Receive,Read,Write}`.
- Support send/recv, RDMA write, and RDMA read; bandwidth and latency; poll mode
  only for native ND.
- Treat Linux perftest-only controls as non-goals for native ND unless ND has a
  real equivalent.

## Starting Point

Current code in `tests/benchmark/native/nd/nd_perftest.cpp` already implements direct
ND2 polling for:

- send/recv bandwidth and ping-pong latency,
- RDMA write bandwidth and latency,
- RDMA read bandwidth and latency,
- single-process and two-process server/client roles,
- common `rdma_bench` option parsing and result JSON output.

Known remaining gaps after the ND-side implementation pass:

- The native ND client hot loops now share backend-neutral benchmark helpers with
  the rest of the benchmark harness. Some secondary server-role result summaries
  remain intentionally lightweight because client-side numbers are the canonical
  unidirectional bandwidth / latency numbers.
- Native ND has its own `signal_ready` and orchestration details that duplicate
  benchmark-core behavior; this is not on the measured data path.
- Compatibility targets `native_nd_send_recv_bench` and
  `native_nd_read_write_bench` still exist for one transition cycle; the
  canonical public benchmark is `nd_perftest` plus the six perftest-shaped
  entrypoints.

## Target Layout

```text
tests/
  benchmark/
    rdma_bench_common.hpp          # options, scenarios, results, timing helpers
    rdma_perftest_engine.hpp       # backend-neutral measurement loops
    asio_perftest_core.hpp         # RDMA-on-Asio adapter + dispatch
    asio_perftest.cpp
    send_recv.cpp
    read_write.cpp
    native/nd/
      nd_perftest.cpp              # native ND adapter/session + dispatch + CLI
```

The exact file split can stay conservative, but the ownership boundary should be
clear: `rdma_perftest_engine.hpp` knows how to measure, while the Asio and native
ND adapters know how to issue and complete operations.

## Shared Engine Design

The shared engine should capture the test-loop shape that is identical across
`asio_perftest` and `nd_perftest`:

- parse options and scenarios,
- validate unsupported knobs,
- run server/client/single-process roles,
- open the warmup -> measured-window transition,
- apply duration and margin rules,
- maintain queue-depth slots,
- track posted/completed counts,
- collect per-op cycle timestamps,
- compute bandwidth peak/average and latency distribution,
- fill CPU metrics and result metadata,
- write JSON plus perftest-style stdout.

Use a compile-time transport policy rather than virtual interfaces in the hot
path:

```cpp
template <class Transport>
rdma_bench::result run_send_bw(Transport& t, rdma_bench::options const& opt);

template <class Transport>
rdma_bench::result run_read_write_bw(Transport& t, rdma_bench::options const& opt);
```

The transport policy owns operation details:

- connection setup and teardown,
- MR registration,
- remote-address / remote-token exchange,
- receive pre-posting,
- `post_send`, `post_recv`, `post_write`, `post_read`,
- completion polling and status translation,
- validation hooks after the measured window.

This keeps the benchmark loop aligned without forcing native ND to depend on the
RDMA-on-Asio public API.

## Measurement Semantics

- Setup, address exchange, MR registration, warmup, teardown, and validation stay
  outside the measured window.
- Send/recv bandwidth: client sends, server receives; receives are pre-posted and
  replenished so the receive window does not underrun.
- Send/recv latency: one outstanding ping-pong; report one-way latency as
  half-RTT, matching `asio_perftest`.
- RDMA write/read bandwidth: client initiates and counts payload bytes.
- RDMA read latency: use the same read-latency convention as `asio_perftest`
  (`rtt_factor=1` in the cycle-counter stats).
- RDMA write latency: use the same convention as `asio_perftest`; do not invent a
  separate native-only interpretation.
- Native ND supports poll mode only. If a scenario requests event mode,
  `nd_perftest` returns a capability skip.
- Result files must be treated as artifacts under `tests/benchmark/results/` or a
  caller-provided output directory; they are not source files.

## Stages

### Stage 0 -- Plan hygiene and migration contract

- Build targets are controlled by `RDMA_BUILD_NATIVE_BASELINES`; hardware CTest
  registration is controlled separately by `RDMA_ENABLE_HARDWARE_TESTS` and
  `RDMA_TEST_ADDR`.
- The direct-ND benchmark implementation lives under
  `tests/benchmark/native/nd/` because it is benchmark infrastructure rather
  than a reusable native-ND test fixture.
- The first implementation keeps `nd_perftest.cpp` as a single native-ND
  adapter/session/dispatch file while adding the canonical `nd_perftest` target
  and thin entrypoints. This gives users the new interface without prematurely
  splitting files before the shared-engine boundary settles.
- Every extraction step must preserve the direct ND2 operation sequence unless the
  stage explicitly says the measured window changes.
- The old target names are compatibility aliases only; new tooling should call
  `nd_perftest` or the `nd_*_{bw,lat}` entrypoints.

### Stage 1 -- Rename and target shape -- initial implementation done

- Add a canonical `nd_perftest` target.
- Add perftest-shaped entrypoints: `nd_send_bw`, `nd_send_lat`, `nd_write_bw`,
  `nd_write_lat`, `nd_read_bw`, and `nd_read_lat`. The first implementation may
  build these from the same `nd_perftest.cpp` source using compile-time presets;
  separate tiny entry source files are optional cleanup, not a correctness
  requirement.
- Keep `native_nd_send_recv_bench` and `native_nd_read_write_bench` as aliases for
  one transition cycle, then remove them.
- Preserve `baseline=native_nd`, `backend=nd`, and poll-only behavior.

Implemented in the initial pass: `nd_perftest`, the six `nd_*_{bw,lat}`
entrypoints, and compatibility binaries for the old target names. The direct ND
core now lives in `tests/benchmark/native/nd/nd_perftest.cpp`.

### Stage 2 -- Backend-neutral benchmark engine -- initial ND-side implementation done

- **Stage 2a -- accounting helpers first:** extract only small backend-neutral
  helpers for prime-count calculation, warmup/duration completion accounting,
  result finalization, and stdout/JSON consistency. Do not move connection setup,
  MR registration, or transport calls in this slice.
- **Stage 2b -- bandwidth loop template:** extract the queue-depth driven
  post/complete loop behind a compile-time transport policy. The policy exposes
  `post(slot)`, `wait(slot)`, `complete(slot)`, and optional validation hooks.
- **Stage 2c -- latency loop template:** extract the one-outstanding ping-pong
  loop once send/recv and read/write latency have matching sentinel / barrier
  semantics.
- Start with poll-mode send/recv bandwidth and latency because the transport
  state is the simplest, then move read/write.
- Keep each extraction numbers-neutral unless the stage explicitly changes the
  measured window. The first non-neutral work is measurement-fidelity parity in
  Stage 5.
- Keep the engine header backend-neutral: no `rdma/rdma.hpp`, no `ndsupport.h`,
  and no Asio completion tokens in the engine itself.

Implemented:

- Added `tests/benchmark/rdma_perftest_engine.hpp` with backend-neutral
  `prime_count`, `planned_total_ops`, `run_bandwidth_window`, and
  `run_latency_window`.
- The engine owns only warmup/duration accounting, cycle timestamp collection,
  result finalization, CPU metrics, and peak/latency stats. Transport post/wait
  calls stay in backend lambdas, so there is no native ND dependency in the
  engine and no virtual/type-erased hot path.
- Native ND client send/recv/read/write bandwidth and latency now call the shared
  helpers. Asio paths already share the same `window_controller` and result
  helpers; moving every async/event role into the same template is a later
  mechanical cleanup, not required for the native ND baseline to align.

### Stage 3 -- Native ND send/recv on aligned measurement semantics -- done

- **Stage 3a -- client measurement parity:** move native ND send/recv client
  bandwidth and latency to `window_controller`, `finish_bw_cycles`, and
  `fill_latency_cycles`.
- **Stage 3b -- server termination protocol:** add an explicit end-of-stream
  sentinel so duration-mode send/recv does not require the server to know the
  client-side operation count in advance. Normal payloads remain exactly
  `message_size` bytes; the sentinel is a one-byte control send outside the
  measured window.
- **Stage 3c -- receive-window parity:** keep the server receive window full by
  pre-posting up to queue depth and replenishing every normal receive until the
  sentinel arrives. A short non-sentinel receive is an error.
- **Stage 3d -- extraction:** once 3a-3c are stable, move the loop shape into
  the shared engine from Stage 2.
- Report the same JSON fields and stdout columns as `asio_perftest`.
- Preserve `asio_perftest` accounting semantics: `completed_count` excludes
  warmup, while `posted_count` records issued operations according to the
  existing result schema. Server-side sentinel receives stay outside measured
  payload counts.

Progress:

- Native ND send/recv client bandwidth now uses `window_controller` and
  `finish_bw_cycles`.
- Native ND send/recv client latency now uses cycle-counter deltas and
  `fill_latency_cycles`.
- Duration-mode send/recv now has the same explicit one-byte end-of-stream
  sentinel shape as `asio_perftest`, so the server does not need to know the
  client-side operation count in advance.
- Server receive pre-posting is replenished during bandwidth runs until the
  requested count is served or the duration sentinel arrives.
- The client hot loop now uses `rdma_perftest_engine.hpp`.
- A two-process native ND send/recv poll smoke run completed with
  `completed_count == iterations`, `warmup_iterations` preserved, and no errors.

### Stage 4 -- Native ND read/write on the shared engine -- done

- Move direct ND read/write bandwidth and latency to the shared engine.
- Move read-bandwidth validation out of the timed loop and validate after the
  measured window.
- Keep write validation on the server after disconnect / post-window completion.
- Preserve remote address/token exchange using direct ND control messages or
  private data; do not route through RDMA-on-Asio helpers.

Implemented: read-bandwidth validation has been moved out of the timed loop, and
native read/write client bandwidth and latency now use the shared
`rdma_perftest_engine.hpp` helpers. The direct ND remote address/token exchange
remains native and does not route through RDMA-on-Asio helpers.

### Stage 5 -- Measurement-fidelity parity with asio_perftest -- done for native ND

- Use `window_controller` for warmup, duration, margin trimming, and in-window
  counts.
- Use `fill_latency_cycles` and `finish_bw_cycles` for cycle-counter latency and
  bandwidth stats.
- Fill `cpu_util_percent`, `cpu_cycles_per_op`, and `context_switches` where the
  platform supports them.
- Keep validation outside the measured window and expose `validation_passed`.
- Gate non-default `inline_size`, `cq_mod`, `post_list`, `recv_post_list`,
  `signaled_every`, and `qps>1` the same way `asio_perftest` does until both
  sides have a real equivalent.

Progress:

- Read/write client bandwidth and latency already use `window_controller` and
  cycle-counter result helpers.
- Send/recv client bandwidth and latency now use `window_controller` and
  cycle-counter result helpers.
- Read-bandwidth validation is outside the measured window.
- Native ND rejects event mode and unsupported perftest knobs through explicit
  skip results.
- Scenario parsing now applies `duration_sec`, `margin_sec`, and
  `warmup_iterations`, so scenario-driven ND/asio comparison runs do not silently
  drop the timing-window controls.
- CPU metrics are collected after the warmup boundary in the shared client
  helpers. Windows still reports `context_switches=null`, matching platform
  support.

### Stage 6 -- Windows comparison runner -- done

- Add a Windows-friendly runner that executes:
  - `asio_perftest --backend nd --mode poll`,
  - `nd_perftest --mode poll`.
- Reuse the same scenario files where possible.
- For event-mode scenarios, run only the RDMA-on-Asio side and emit an explicit
  native-ND skip.
- Produce a table with `baseline=rdma_on_asio` and `baseline=native_nd` rows for
  each operation/metric/size/queue-depth.
- Keep generated comparison artifacts under an ignored output directory.

Implemented: a PowerShell `run_nd_comparison.ps1` runner drives `asio_perftest`
and `nd_perftest` over the same scenario and produces a comparison table. A
short single-host comparison smoke run completed successfully; long-form result
collection remains a benchmark artifact task, not source-code work.

### Stage 7 -- ND backend parity prerequisites -- done

These are adjacent library/backend tasks needed for fair ND benchmarking:

- Apply `rdma_config_t::cq_poll_batch_` in the ND backend: derive a default of 16,
  size `IND2CompletionQueue::GetResults` buffers from the effective value, and
  pass it through `nd_use_device` into the ND IO-completion service.
- Document `rnr_retry_` and `min_rnr_timer_` as IBV-only. ND2 does not expose an
  RNR NAK timer or retry count; do not invent an ND equivalent.
- Verify ND send/recv two-process streaming with a full receive window so the
  plan has evidence that ND provider defaults do not introduce the IBV-style RNR
  stall previously observed.

Implemented: `cq_poll_batch_` is now derived and used by the ND standalone and
event CQ pollers, and the shared RNR comments document that ND ignores the
IBV-only knobs. A two-process native ND send/recv streaming smoke run completed
without an RNR-style stall or receive-window underrun.

## Guardrails

- Do not let `nd_perftest` depend on the RDMA-on-Asio public API. Shared benchmark
  headers are fine; `rdma/rdma.hpp`, `async_send`, `async_recv`, `async_read`, and
  `async_write` are not part of the native ND data path.
- Do not include Asio-RDMA-specific helpers in the backend-neutral engine. Keep
  `rdma_perftest_engine.hpp` free of `rdma/rdma.hpp`; backend adapters provide the
  concrete operations.
- Do not add virtual calls, `std::function`, heap allocation, or type-erased
  callbacks inside the hot post/complete loop. Use templates or CRTP so each
  target compiles its own direct loop.
- Do not implement a native ND IOCP/event baseline just to match
  `asio_perftest` event mode. Native ND is the direct poll baseline; RDMA-on-Asio
  event mode measures scheduler integration.
- Do not fake Linux perftest-only knobs on ND. `--gid-index`, `--rdma_cm`, IB MTU
  selection, RNR retry/timer, and similar verbs concepts should be recorded as
  not applicable or rejected in strict mode unless ND has a real equivalent.
- Do not leave validation, logging, JSON writing, remote-info exchange, or memory
  registration inside the measured window.
- Do not compare `nd_perftest` numbers with Linux perftest as if they were the
  same baseline. The valid comparison is RDMA-on-Asio ND vs direct ND2 on the same
  Windows ND platform.
- Do not commit generated benchmark results. Keep raw JSON/stdout/stderr under an
  ignored artifact directory.

## Acceptance Criteria

- `nd_perftest` builds only when `RDMA_BACKEND=nd` and
  `RDMA_BUILD_NATIVE_BASELINES=ON`.
- `nd_perftest` hardware smoke tests register only when
  `RDMA_ENABLE_HARDWARE_TESTS=ON` and `RDMA_TEST_ADDR` is set.
- The native ND data path remains direct ND2 and poll-only.
- `nd_perftest` and `asio_perftest` share the benchmark control framework and
  result schema.
- For poll-mode send/recv/read/write bandwidth and latency, the two tools can be
  driven by the same scenario dimensions.
- Event-mode native ND requests produce explicit skip results.
- Read/write validation is outside the measured window.
- The comparison runner can produce a two-row table for each ND scenario:
  RDMA-on-Asio ND and native ND.

