# RDMA Stress, Performance, And Latency Plan

A separate, hardware-only, opt-in suite for: stress-testing the RDMA-on-Asio
state machines under concurrency; measuring send/recv and RDMA read/write
throughput and latency; and comparing against `linux-rdma/perftest` (IBV) and a
native NetworkDirect baseline (ND). Kept apart from `unit_test_plan.md`, which
owns small, deterministic correctness tests.

The measurement tool is **`asio_perftest`** -- a benchmark project built as a
deliberate mirror of perftest (same CLI, same run/verbs-driving structure) that
differs on exactly one axis: the data path goes through the RDMA-on-Asio public API
instead of raw verbs. That makes the head-to-head delta a controlled measurement of
the abstraction's cost against an industry-standard baseline (see
[asio_perftest: Feature Parity With perftest](#asio_perftest-feature-parity-with-perftest)).

## Scope Split

- **`unit_test_plan.md` owns correctness** (pass/fail by exact rules): RDMA r/w
  round trip, zero-length send/recv, multi-message ordering, negative connect,
  and the private-data / SGL / cancellation / disconnect integration tests.
- **This plan owns stability and measurement signals**: multi-thread shared-CQ
  stress, multi-QP over one event-mode CQ, connect/disconnect soak, send/recv and
  read/write bandwidth + latency, poll-vs-event comparison, and RDMA-on-Asio vs
  perftest / native-ND comparison.

## Library Capability Prerequisites

Some knobs perftest relies on for its tuned defaults are **not implemented in the
current RDMA-on-Asio API**. A benchmark that silently falls back to the library
default while perftest uses its tuned default produces an unfair comparison that
looks like abstraction cost but is a feature-parity gap. **This iteration does not
add any of these capabilities** (no interface/impl change for parity -- see "This
iteration's scope"). Each unsupported knob is handled by `asio_perftest` returning a
`not_implemented` skip for a non-default value (`rdma_bench::not_implemented_reason`),
and the comparison runs perftest at the matching default. **This table is the single
source of truth for the parity constraints referenced throughout this plan.**

| Knob | perftest default | Current library state | This iteration |
|------|------------------|-----------------------|----------------|
| `cq_mod` / selective signaling | unsignaled, CQE every `--cq-mod` | all sends `IBV_SEND_SIGNALED` (`ibv_ops_verbs.hpp`), one CQE per WR | not implemented (skip `--cq-mod>1`); run perftest `--cq-mod 1`. Future: Stage 11.3 |
| `inline_size` / `IBV_SEND_INLINE` | inline for small msgs | inline forced off: `max_inline_data = 0` (`ibv_service_verbs.hpp`); WR never sets `IBV_SEND_INLINE` | not implemented (skip `--inline_size>0`); run perftest `--inline_size 0`. Future: Stage 11.1 |
| `post_list` / `recv_post_list` | batches N WRs per post | `async_send` posts one WR (with an SGL); no multi-WR batching | not implemented (skip `>1`); map `post_list=1` only. Future: Stage 11.2 |
| `--qp` / multi-QP | N QPs over one connection | one cm_id == one QP (`async_connect(qp, ...)`) | not implemented (skip `qps>1`); run perftest `--qp 1`. Future: needs a connector interface change |
| `retry_count` / `rnr_retry` / `min_rnr_timer` | configurable | **configurable** since the RNR fix: `rnr_retry_` (7) / `min_rnr_timer_` (12) in `rdma_config_t`, applied via `ibv_modify_qp` at RTS | done (internal optimization, no public-API change) |

The first honest IBV-vs-perftest bandwidth comparison runs perftest with
`--cq-mod 1 --inline_size 0`, no `--post_list`, and `--qp 1` (one signaled,
non-inline WR per message, a single QP on both sides) -- this iteration's default
parity-constrained run. When a capability is added in a future iteration,
re-baseline rather than comparing across the API change.

## Build, CTest, And Layout

Switches (all default OFF; added when the first program in each group lands):

- `RDMA_BUILD_STRESS_TESTS`, `RDMA_BUILD_PERFORMANCE_TESTS`,
  `RDMA_BUILD_NATIVE_BASELINES`, `RDMA_ENABLE_PERFTEST_BASELINE`,
  `RDMA_BUILD_PERFTEST`, `RDMA_ENABLE_HARDWARE_TESTS` (outer hardware gate).
- `RDMA_PERFTEST_MODE=system` (when baseline enabled), `RDMA_PERFTEST_BIN_DIR`,
  `RDMA_TEST_ADDR`, `RDMA_TEST_PEER_ADDR`, `RDMA_TEST_BASE_PORT`.

CTest labels: `stress`, `performance`, `latency`, `baseline`, `hardware`,
`manual`, and backend scope `nd` / `ibv` / `rdma`. **Default `ctest` must not run
stress/performance/latency** -- they are explicitly selected
(`ctest -L "performance|latency"`, `ctest -L stress`). Use Release for numbers;
Debug only for stress correctness / sanitizers.

Current layout (create a directory only when its first real program lands):

```text
tests/
  stress/rdma/        shared_cq.cpp, connect_disconnect_soak.cpp   (stability/race, not perf)
  native/nd/          native_nd_baseline.cpp
  benchmark/          -- the asio_perftest project (all of it):
    rdma_bench_common.hpp, asio_perftest_core.hpp, asio_perftest_clock.hpp
    asio_perftest.cpp                  (dispatch + cli_main)
    send_recv.cpp, read_write.cpp      (verb modules: no main, export run_send_recv/run_read_write)
    entries/          asio_perftest_main.cpp + asio_{send,read,write}_{bw,lat}.cpp
    scenarios/        smoke.json, baseline.json, sweep.json, read_write_smoke.json
    tools/            run_scenario.cpp, compare_results.cpp,
                      perftest_commands.cpp, parse_perftest.cpp, capability_probe.cpp
```
(Stage 9a folded the old `performance/rdma/{send_recv,read_write}.cpp` into the
asio_perftest project under `benchmark/`; `tests/performance/` no longer exists.
`stress/` stays separate -- it is stability/race coverage, not a perf baseline.)

**Benchmark target naming:** the perftest-aligned benchmark is the `asio_perftest`
target family -- an umbrella `asio_perftest` (multiplexed by `--operation`/
`--metric`) plus the six perftest-shaped entrypoints `asio_{send,read,write}_{bw,lat}`.
Stage 9a unifies the legacy `rdma_send_recv_bench` / `rdma_read_write_bench` into
this family (old names kept as aliases for one cycle, then removed).

## perftest Baseline

### Dependency policy and modes

`linux-rdma/perftest` is the IBV verbs baseline. It must be available to the
benchmark workflow but **never a dependency of the library** (subprocess only,
never linked; `RDMA_ENABLE_PERFTEST_BASELINE`/`RDMA_BUILD_PERFTEST` default OFF).
Three modes:

- `system` (default): find installed `ib_*` on `PATH`.
- `external`: `--perftest-bin-dir=<path>` / `RDMA_PERFTEST_BIN_DIR` (user-built);
  `--perftest-source-dir=<path>` records the source commit and uses its build.
- `managed`: build a project-managed checkout only when `RDMA_BUILD_PERFTEST=ON`.

Ownership boundary: RDMA-on-Asio tools measure this library; perftest measures
direct verbs; comparison tooling normalizes and compares. On Windows, perftest
normally probes as unavailable -> use the native ND baseline instead.

**Decision: system install is the default; a pinned submodule is the opt-in
`managed` mode only.**

| Factor | System install (`apt install perftest`) | Submodule (`third_party/perftest`) |
|---|---|---|
| Setup | one command; packaged on every RDMA distro (Ubuntu 24.04 -> 24.01) | clone + autotools build toolchain |
| License | perftest is GPL-2.0; no vendoring | vendors a GPL tree (safe -- subprocess only -- but heavier) |
| Reproducibility | distro version, can drift | exact pinned commit |

A submodule's only edge is version pinning, better served by **recording the
perftest version/commit in result metadata** than by vendoring a GPL autotools
tree the library never links. Hence `system` -> `external` -> `managed`.

**No native IBV baseline.** A native (raw-verbs) baseline exists only where no
authoritative external one does. perftest *is* the IBV verbs baseline, so the repo
must not add `tests/native/ibv/`. The native ND baseline exists solely because
Windows NetworkDirect has no perftest equivalent.

### Comparison strategy

perftest is a baseline, not a pass/fail oracle. The canonical operation mapping
(also the IBV execution matrix) -- each row runs under identical message-size /
queue-depth / inline / cq-mod / connection settings, perftest constrained to
`--connection RC --cq-mod 1 --inline_size 0 --rdma_cm`, no `--post_list`:

| # | Operation | Metric | RDMA-on-Asio | perftest | Notes |
|---|---|---|---|---|---|
| 1 | send/recv | bandwidth | `rdma_send_recv_bench` | `ib_send_bw` | poll first; event -> `--events` both sides |
| 2 | send/recv | latency | send/recv ping-pong | `ib_send_lat` | perftest = half-RTT; compare to req/resp |
| 3 | RDMA write | bandwidth | `rdma_read_write_bench` (write) | `ib_write_bw` | |
| 4 | RDMA write | latency | write ping-pong | `ib_write_lat` | |
| 5 | RDMA read | bandwidth | `rdma_read_write_bench` (read) | `ib_read_bw` | localize the ~2.4 Gbit/s IBV read ceiling here |
| 6 | RDMA read | latency | read ping-pong | `ib_read_lat` | |

Rules: compare IBV first (perftest is verbs-oriented) and RC transport first;
poll mode first (perftest default polling is the closest baseline; event mode vs
`--events`); same hosts / ports / GID / MTU / size / queue depth / CPU affinity;
same size sweep; same queue depth for bandwidth, one outstanding op for latency;
identical server/client command lines except the peer address; run both iteration
and duration modes. UC/UD only after the public API supports them.

- **GID / RoCE-version parity.** On RoCE the GID index selects the RoCE version
  (v1 vs v2) and the source IP. GID indices are **node-local**, so the thing that
  must match across the two tools is the RoCE *version and address family*, not
  the index number (the same v2 GID may sit at a different index on each host, and
  perftest does not enforce a shared index). The library auto-selects a GID;
  record which RoCE version/address it resolves to and pass perftest the
  `--gid-index` that resolves to the same version/address, otherwise the two tools
  run on different transports and the comparison is meaningless. Capture the
  resolved GID (index + RoCE version + address) in the result metadata for both
  sides.
- **Connection-setup caveat.** The library is rdma_cm-only, so perftest must use
  `--rdma_cm` (`-R`) for a like-for-like control plane. perftest's *default* is a
  socket out-of-band QP-info exchange, which `asio_perftest` cannot replicate at
  all -- do not present a socket-setup perftest run as comparable. Setup is
  excluded from the measured window, so this matters only for connect/setup
  benchmarks, not steady-state throughput.

### Command templates

The runner generates commands from scenarios (never bakes them into CTest):

```text
# bandwidth (send; write/read identical with ib_write_*/ib_read_*)
ib_send_bw [<server_ip>] --ib-dev <dev> --ib-port <port> --gid-index <gid> \
  --mtu <mtu> --connection RC --size <bytes> --duration <sec> --margin <sec> \
  --tx-depth <depth> --rx-depth <depth> --cq-mod <n> --inline_size <bytes> \
  --port <tcp_port>
# latency
ib_send_lat [<server_ip>] ... --connection RC --size <bytes> --iters <count> \
  --inline_size <bytes> --port <tcp_port>
```

`--rdma_cm` (`-R`) is **required**, not optional -- it is in the fixed constraint
string above and the only control plane the library has (see Connection-setup
caveat). Optional dims: `--events` (event mode), `--qp` (multi-QP),
`--report-histogram`/`--report-unsorted` (latency distribution). `-b`
bidirectional and `--post_list`/`--recv_post_list` stay out until the library has
the equivalent (see Capability Prerequisites).

**Prefer perftest's structured JSON over scraping stdout.** v26.04 emits a
machine-readable result file via `--out_json --out_json_file <path>`;
`parse_perftest` should consume that when present (stable field names across
versions) and fall back to the stdout "last numeric row" heuristic only when a
build lacks JSON output. Either way keep raw stdout/stderr as the archived source
of truth, and on a parse miss report a parser error rather than inventing numbers.

### Feature inventory (reviewed from /data/cpp/perftest, v26.04.17)

The surface `asio_perftest` is measured against (139 long options):

- **Tools:** `ib_{send,read,write,atomic}_{bw,lat}`,
  `raw_ethernet_{bw,lat,burst_lat,fs_rate}`, `clock_test`.
- **Verbs:** SEND, SEND_IMM, WRITE, WRITE_IMM, READ, ATOMIC (CMP_AND_SWAP,
  FETCH_AND_ADD). **Transports:** RC, UC, UD, RawEth, XRC, DC, SRD.
- **Test type/method:** LAT / BW / LAT_BY_BW / FS_RATE; iterations (`-n`) or
  duration (`-D`); `--run_infinitely`.
- **Per-message knobs:** `--inline_size`, `--cq-mod`, `sig_before`,
  `--post_list`/`--recv_post_list`, `--qp`, `--mr_per_qp`, `--use-srq`,
  `-b`+`--report-both`, `*_with_imm`, `--rate_limit`, `--odp`, `--use_rss`.
- **Fabric/QP:** `--mtu`, `--gid-index`, `--pkey_index`, `--sl`, `--tos`,
  `--tclass`, `--flow_label`, `--hop_limit`, `--dualport`, `--qp-timeout`,
  `--retry_count`; setup via socket (default) or `--rdma_cm` (`-R`).
- **Memory:** host (default), CUDA/ROCm/MLU/Neuron/OpenCL, device-memory,
  hugepages, null-mr, DMABUF.
- **Output:** human table (`#bytes #iterations BW peak/average[Gb/sec|MiB/sec]
  MsgRate[Mpps]`; latency `t_min t_max t_typical t_avg t_stdev 99% 99.9%`),
  `--out_json`, `--output bw|mr|lat`, histogram, `--cpu_util`.
- **Methodology:** for send/recv and write, latency is round-trip reported as
  half (one-way, `rtt_factor=2`); for **read and atomic the value is NOT halved**
  (`rtt_factor=1`) -- the one-sided op captures the full round-trip in a single
  completion, so case 6 (read lat) compares against the un-halved `ib_read_lat`.
  `t_typical` is the median; on unidirectional BW the client measures; first
  sample is the warmup peak.

### Per-run metadata

Every baseline run records: perftest path + git commit/package version; full
command line; device, port, GID index, MTU, message size, queue depth, inline
size, CQ moderation, connection mode; host CPU model, NUMA placement, OS, kernel,
driver, firmware where available.

## Measurement Semantics And Result Schema

### Methodology (applies to every measured operation)

- **Token choice is part of the measurement.** Poll mode must use a
  non-`io_context` token (`callback`/`use_future`) so completion fires inline on
  the polling thread; `use_awaitable` posts back to an io_context and measures
  post overhead instead (it is the coroutine-cost measurement). Record the token.
- **Latency sampling.** Percentiles (p50/p90/p99) require a full per-sample array
  or an HDR histogram; running min/avg/max is insufficient. Record the method.
- **Clock calibration.** At RDMA scales the timestamp read is a non-trivial
  fraction of the interval; measure clock-read cost at startup
  (`clock_overhead_ns`), use a monotonic high-res source, pin the measuring
  thread.
- **Roles.** Name the direction: send/recv -> client sends, server receives,
  bytes counted at the sender; RDMA r/w -> client is the initiator.

Per operation: **send/recv bw** -- pre-post receives and continuously replenish
during the window (running out hits RNR retry with `rnr_retry`=7, which silently
inflates latency / depresses throughput; treat any RNR event as
measurement-invalidating); count sender payload bytes; report bytes/s and msg/s.
**send/recv lat** -- ping-pong, report one-way as half-RTT, one outstanding op.
**write/read bw** -- initiator counts its payload bytes, remote buffer stays
pinned and hot. **write/read lat** -- signaled-completion latency on the
initiator, one outstanding op. Setup, MR registration, private-data exchange,
warmup, and validation are kept **outside** the measured window.

### CLI controls

Tools are build-system neutral (CTest may pass defaults from cache vars). Core
options: `--server`/`--client`/`--single-process`, `--local-addr`/`--peer-addr`/
`--port`, `--backend`, `--operation`, `--metric`, `--mode=event|poll`,
`--token-type`, `--message-size`, `--iterations`/`--duration-sec`,
`--margin-sec` (symmetric ramp excluded from both ends of the timed window --
window = duration - 2*margin -- mirroring perftest `--margin`), `--queue-depth`,
`--qps`, `--threads`, `--warmup-iterations`,
`--inline-size`, `--cq-mod`, `--post-list`/`--recv-post-list`, `--signaled-every`,
`--no-peak` (perftest `-N`), `--report-gbits` (Gb/sec vs MiB/sec), `--baseline`,
`--topology`, `--scenario`, `--json-out`, `--raw-stdout`/`--raw-stderr`. The
parser rejects unsupported combinations explicitly (e.g. `--gid-index` is
IBV-specific; ND tools warn in compat mode or reject in strict mode).

### Result schema and units

Versioned, machine-readable JSON from the first implementation. Fields:
`schema_version`, `scenario_name`, `backend`, `baseline`, `topology`,
`operation`, `metric`, `completion_mode`, `token_type`, `message_size_bytes`,
`queue_depth`, `qps`, `threads`, `iterations`, `duration_sec`,
`warmup_iterations`, `payload_bytes`, `posted_count`, `completed_count`,
`throughput_mib_s`, `throughput_gbit_s`, `message_rate_s`, `latency_{avg,min,p50,
p90,p99,max}_us`, `latency_sample_method`, `clock_overhead_ns`,
`cpu_cycles_per_op`, `cpu_util_percent`, `context_switches`, `rnr_retry_events`,
`validation_passed`, `errors`, `first_error`, `exit_code`, `skip_reason`,
`missing_capability`, `command_line`, `environment`.

Units: bytes for sizes, MiB/s for binary throughput, Gbit/s for bit rate,
microseconds for latency; missing values are `null`, not `0`; raw perftest output
is stored next to parsed fields. CPU efficiency is the core of the
abstraction-cost question: at line rate two tools can both saturate the link while
differing greatly in CPU cost, so throughput alone cannot answer "what does the
Asio + coroutine layer cost?". `cpu_cycles_per_op` (cycles per message) is the
asio-specific cost metric with no perftest counterpart; `cpu_util_percent`
(wall CPU%) is the side-by-side number comparable to perftest `--cpu_util`;
`context_switches` is a supporting signal. (See Stage 10 -- do not conflate the
two CPU metrics.)

### Environment capture

Record (and where practical control): Release build + flags, CPU affinity for
server/client/poller, NUMA node, CPU governor / power plan, interrupt affinity,
NIC MTU/link/driver/firmware, hugepages if used, background load, repetitions.
Report per-run values and aggregate min/median/max across >=3 repetitions; do not
claim a regression from a single noisy run.

## Scenarios And Topology

### Scenario files and suites

Scenarios describe intent, not executable flags, keeping perftest / native-ND /
RDMA-on-Asio runs aligned (fields mirror the result schema: name, backend,
baseline list, operation, metric, completion_mode, token_type, connection,
message_size, queue_depth, qps, threads, iterations/duration_sec, margin_sec,
inline_size, cq_mod, post_list, recv_post_list, signaled_every, mtu, gid_index,
topology, cpu_affinity). Dimensions: operations (send/recv, write, read); metrics
(bandwidth, latency); modes (poll first, event second); tokens (`callback` first,
then `use_awaitable` -- the gap is the coroutine cost); sizes 1..65536 (powers of
two); bandwidth queue depths 1/8/32/128/512; latency queue depth 1; QPs 1/2/4/8/16;
threads 1 then one submitter per QP.

Named suites (the full matrix never runs by default): `smoke`, `baseline`,
`sweep`, `scale`, `event`. A scenario is skipped only for a missing capability
(written into the result JSON with the capability name), never because a parameter
was silently ignored; unsupported fields error in strict mode; a perftest option
with no library equivalent stays out of the scenario rather than being enabled on
one side only.

### Topology

**Single-host** (label one of `single_host_same_process` /
`single_host_two_process` / `single_host_dual_port`): same-process loopback (API
overhead, fast iteration), two-process loopback (closest to perftest usage), or
dual-port if NIC/cabling allow. Valid for regression and overhead exploration, not
final network claims. Prioritize repeatability: pin stable CPU sets, avoid sharing
a physical core, record NUMA/NIC locality, warm up, repeat >=3x.

**Multi-host** (label `two_host_direct` / `two_host_switch` / `multi_pair_switch`)
promotes the suite to a real transport comparison. Requirements: documented NIC
generation/firmware, stable link/MTU, NIC-local CPU/NUMA on both hosts, documented
control-plane firewall/routing, identical binary revision/build. Run order: verbs
baseline smoke (perftest) -> RDMA-on-Asio smoke -> latency scenarios -> bandwidth
duration -> event mode -> archive raw logs/JSON/perftest output/env from both
hosts. Extensions: pairwise matrix, one-server many-client fan-in, independent
pair saturation, cross-backend (kept separate unless HW/OS difference is the
experiment).

### Runner, discovery, and skip policy

A small runner orchestrates processes and collects logs without hiding raw
commands: allocate control-plane ports, launch RDMA-on-Asio (and optionally
perftest) server/client, wait for an explicit server-ready line, enforce
startup/measurement/teardown timeouts, terminate both sides on failure preserving
partial logs, and write a manifest (scenario, commands, pids, exit codes,
timestamps, output paths). Multi-host can start manual (print exact per-host
commands, collect results into one directory).

Probe before large suites: IBV devices/ports/link/MTU/speed/GID, ND
providers/adapters/addresses, perftest presence + version, loopback/dual-port
support, free control-plane ports. Skip policy: missing hardware/perftest,
unsupported topology or completion mode -> `skipped`; connection failure after a
valid probe, or watchdog timeout -> failure; perftest parser failure is a
reporting failure but raw output is preserved.

## Native ND Baseline

perftest is verbs-only, so ND uses a native Windows baseline calling ND2 directly
(no RDMA-on-Asio wrappers), in `tests/native/nd/`. This answers the Windows
abstraction-cost question perftest cannot, and is the ND analogue of the IBV-vs-
perftest comparison. Currently implemented (`native_nd_send_recv_bench`,
`native_nd_read_write_bench`): polling send/recv bandwidth + ping-pong latency,
and polling RDMA write/read bandwidth + latency, via direct ND2
provider/adapter/PD/CQ/QP/connector/listener and MR APIs.

Rules:

- Share the scenario + result JSON schema and the common option parsing / raw-log
  archiving / watchdog / report code with the RDMA-on-Asio tools; mark
  `baseline=native_nd`, `backend=nd`; store output beside the RDMA-on-Asio run.
- Build switches: `RDMA_BUILD_NATIVE_BASELINES` + `RDMA_ENABLE_HARDWARE_TESTS`
  gate registration; labels `baseline;native_nd;nd;hardware;manual`.
- Keep buffer registration, queue depth, inline size, message size, CPU placement
  aligned with the RDMA-on-Asio ND run; setup/registration/teardown and validation
  outside the measured window.
- **Poll-mode only by design**: RDMA-on-Asio event mode is the event-scheduler
  measurement; native ND IOCP/event is out of scope.
- For RDMA r/w, exchange `rdma_remote_addr_t` via a small control message /
  private data; validate write on the server, read on the client.

This yields three comparable tracks: IBV RDMA-on-Asio vs perftest; ND
RDMA-on-Asio vs native ND; and IBV vs ND RDMA-on-Asio as a portability/abstraction
signal (not a hardware-equivalent comparison unless HW/link/host match).

## ND backend parity (TODO for the NetworkDirect agent)

The ibv backend just gained three changes; nd (Windows NetworkDirect) must follow
to stay aligned. `rdma_config_t` is shared across backends, so the new fields
already exist for nd -- the nd agent acts on, or explicitly documents, each.

1. **CQ poll batch (`cq_poll_batch_`).** nd's `nd_config_derive.hpp` fills
   cqe/wr/sge/inline/read_limit but not this -- add
   `if (cq_poll_batch_ == 0) cq_poll_batch_ = 16;` (mirror ibv's
   `default_cq_poll_batch`). Size the `IND2CompletionQueue::GetResults` result
   array from the effective `cq_poll_batch_` in `nd_completion_queue.hpp` /
   `nd_service_io_completion.hpp` (replace the hard-coded reap count), and pass it
   through `nd_use_device` into the io-completion service (mirror ibv's
   `initialize(..., poll_batch, ...)`).

2. **RNR knobs (`rnr_retry_` / `min_rnr_timer_`) -- treat as ibv-only.** ibv added
   these because rdma_cm leaves `min_rnr_timer` at its ~655 ms default, which stalls
   send/recv on a receive-window underrun. ND2's `IND2Connector::Connect` exposes
   inbound/outbound read limits (the `read_limit` analogue) but does **not** expose
   an RNR NAK timer or retry count -- those are IB-verbs concepts the ND provider
   owns internally. So nd **ignores** `rnr_retry_` / `min_rnr_timer_` (note this in
   the `rdma_commons.hpp` field comments); do not invent an ND equivalent.
   **Must verify**: run nd send/recv bandwidth 2-process (streaming, full receive
   window) and confirm it does NOT stall the way ibv did before the fix -- prove
   ND's provider defaults are sane rather than assume it (ibv hid a 655 ms pothole
   exactly here).

3. **Read-bw validation out of the measured window.** The asio_perftest verb
   modules (`send_recv.cpp` / `read_write.cpp`) are cross-platform, so nd builds
   inherit this fix automatically. But `tests/native/nd/native_nd_baseline.cpp` has
   the same bug the ibv bench had: its read-bandwidth loop calls `verify_read(slot)`
   per op inside the timed window (between `begin` and `finish_throughput`), pinning
   read bw to the per-message memcmp rate. Move it out (validate once after the
   timed window, as `read_write.cpp`'s `finish_success` now does).

4. **Config-field consistency.** Resolve each shared field explicitly:
   `cq_poll_batch_` -> applied to the ND CQ reap; `rnr_retry_` / `min_rnr_timer_`
   -> documented ibv-only. Update nd's `is_config_compatible` if it range-checks
   config fields.

## asio_perftest: Feature Parity With perftest

### This iteration's scope (measure + internal-optimize only)

This iteration deliberately does **not** change the rdma-on-asio public interface or its
implementation in order to reach perftest parity. The work is limited to:

- **Performance measurement** -- the lib-vs-perftest comparison (Stage 8), the bench-layer
  parts of run-structure parity (Stage 9b: warmup loop, phase barrier, `--margin-sec`,
  constraints column), and measurement-fidelity (Stage 10). Stage 9b's `--qp` multi-QP knob
  is **deferred** -- it needs a connector interface change (one cm_id == one QP today), so
  `qps > 1` returns a not-implemented skip; see Stage 9b and the matrix.
- **Internal-implementation optimizations** that touch no public API -- the min_rnr_timer
  fix already landed; candidates: the single-buffer SGE fast path, event-mode poller
  drain/re-arm tuning.

Perftest features that would require interface/impl changes are **out of scope this
iteration**. `asio_perftest` still accepts the matching flags for command-line parity, but a
non-default value it cannot honor through the current public API returns a **`not_implemented`
skip** (`rdma_bench::not_implemented_reason` -> `make_skip_result`, gated in
`asio_perftest.cpp::cli_main`) instead of a misleading number. Stage 11 and Stage 12 below
keep their designs as the future-iteration reference.

| perftest feature | rdma-on-asio | this iteration |
|---|---|---|
| send/recv, write, read | supported | benchmarked |
| bandwidth / latency | supported | benchmarked |
| event (`--events`) / poll (busy-poll) | supported | benchmarked (perftest WRITE has no `--events`) |
| RC connection, `--rdma_cm` | supported | benchmarked |
| `--inline_size > 0` | needs QP `max_inline_data` + `IBV_SEND_INLINE` | not implemented (CLI skip) |
| `--cq-mod > 1` / `--signaled-every > 1` | needs selective signaling (breaks 1:1 CQE<->op) | not implemented (CLI skip) |
| `--post-list > 1` / `--recv-post-list > 1` | needs a WR-batching API | not implemented (CLI skip) |
| `--qp` / `--qps > 1` (multi-QP) | needs N QPs over one connection (connector is one cm_id == one QP) | not implemented (CLI skip) |
| `-b` bidirectional | needs bidirectional orchestration | not implemented |
| `*_with_imm` | needs an immediate-data API | not implemented |
| UC / UD connection | RC only (hard-coded `IBV_QPT_RC`) | not implemented (`--connection` rejects non-RC) |
| atomics (`ib_atomic_*`) | no atomic operations | not implemented (no operation/entrypoint) |

### Design principle: a perftest mirror, differing on one axis only

`asio_perftest` (the benchmark CMake project) is built as a deliberate **mirror of
perftest**, so that comparing the two is a controlled experiment rather than two
unrelated tools that happen to move bytes. It aligns with perftest on **two axes**:

1. **Command-line surface** -- the same tool shape (`asio_{send,read,write}_{bw,lat}`),
   the same flags (`--size`/`-s`, `--iters`/`-n`, `--tx-depth`/`--rx-depth`, `-c RC`,
   `-R`, `-D`, `-N`, ...), so the same command line drives both.
2. **Measurement structure / how the verbs are driven** -- the same run shape:
   post WRs to a depth, **poll the CQ inline on the measuring thread**
   (perftest's `run_iter_bw`/`run_iter_lat`), with warmup -> pre-window barrier ->
   timed window -> post-window barrier -> result exchange, and a two-process
   out-of-band handshake (perftest's `ctx_hand_shake` + `catch_alarm` phase
   barriers).

It differs from perftest on **exactly one axis**: the data path issues operations
through the **RDMA-on-Asio public API** (`async_send`/`async_recv`/`async_read`/
`async_write` + a completion token) instead of raw `ibv_post_send`/`ibv_poll_cq`.
Because that is the *only* controlled difference, the measured delta between
`asio_perftest` and perftest under identical constraints **is** the abstraction
cost (Asio + coroutine + the QP/CQ/MR wrappers) -- not a methodology gap. perftest
is the industry-standard verbs micro-benchmark, so a relative comparison against it
is far more credible than self-reported numbers.

**Corollary -- model the core on perftest's structure, not on incidental asio
plumbing.** `cq_spinner` and `signal_ready` (in `asio_perftest_core.hpp`) are
internal helpers for the `use_future` poll path and the single-process convenience
mode; perftest has **neither** (it polls inline on the measuring thread and is
always two processes). They are kept as shared helpers but are **not** the
measurement model. The canonical poll-mode comparison is the **inline same-thread
(callback / `poll_until`) path**, which matches perftest's `run_iter_*`; the
`use_future` + `cq_spinner` path is a separate, explicitly-labeled variant
measuring cross-thread/future overhead, not the baseline.

The RDMA-on-Asio benches are effectively the "asio side" of the comparison. Today
two binaries (`rdma_send_recv_bench`, `rdma_read_write_bench`) multiplexed by
`--operation`/`--metric`/`--mode` already cover RC, host memory, send/recv + read
+ write, bandwidth + latency, event + poll, iterations + duration, queue depth,
multi-QP (`--qps`), multi-thread, and server/client/single-process roles, emitting
JSON + a `RDMA_BENCH_READY` handshake. `asio_perftest` is the unified,
perftest-shaped evolution of these. Note two things the current code does **not**
do despite parsing the options: there is **no warmup loop** (`warmup_iterations` is
dead -- every iteration is measured) and **no per-phase sync barrier** (only an
initial server-ready handshake); both are run-structure work, not done. Gaps by
what closes them:

- **Class A -- exposable now (harness only):** unify into one core; perftest-style
  stdout table; perftest flag aliases; the 6 RC cases already map 1:1 to
  `ib_{send,read,write}_{bw,lat}`.
- **Class B -- needs library work (= Capability Prerequisites):** `--inline_size`,
  `--cq-mod`+selective signaling, `--post_list`/`--recv_post_list`, `-b`
  bidirectional, `*_with_imm`. Until each lands, `asio_perftest` self-constrains to
  match (the same clamp the perftest side gets).
- **Class C -- out of scope / non-goals:** raw Ethernet, GPU/DMABUF/device memory,
  ODP, packet pacing, RSS, XRC/DC/SRD, encryption, multicast, dualport,
  `clock_test`. Not abstraction-cost questions.
- **Class D -- measurement fidelity:** full latency stats (`t_min/t_max/t_typical
  (median)/t_avg/t_stdev/p99/p99.9` vs the current p50/p99), cycle-counter timing,
  histogram, MiB-vs-Gb toggle, wired `--cpu_util`, peak-vs-average / `-N`,
  `--run_infinitely`.

UC/UD and atomics straddle B/C: pursue only if the public API grows those modes;
otherwise document as non-goals so the comparison scope stays honest.

The unification splits into two stages with different risk profiles. **Stage 9a is
the only part that may claim "numbers unchanged"; Stage 9b deliberately changes the
measured window and resets the baseline.** Conflating them hides regressions, so
they must not share a commit boundary.

### Stage 9a -- Mechanical unification (numbers-neutral, gated)

Pure Class A / cosmetic-D: no library or data-path change, and crucially **the
measured-window boundaries stay byte-for-byte where they are today** (per the code,
these differ by verb x mode -- e.g. send/recv-bw client times from the in-band 'R'
ready byte, the server from the first received message; read/write-bw client times
right after connection setup; document each start/stop point and preserve it).

- Extract a shared `asio_perftest_core` from `send_recv.cpp` (~1570 lines) and
  `read_write.cpp` (~820 lines) -- one server/client/single-process orchestrator
  (the duplicated create-io_context/atomic-remaining/done-lambda/watchdog/
  select-best/write_result pattern), the `cq_spinner` and `signal_ready` helpers
  (today copied verbatim in both files), and the measurement loop, parameterized by
  verb x metric x mode. Preserve the existing token sub-variants (send/recv poll has
  both `callback` via `poll_until` and `use_future` via `cq_spinner`; read/write
  poll is `use_future` only).
- Provide perftest-shaped entrypoints (`asio_{send,read,write}_{bw,lat}`, thin
  shells presetting operation+metric) plus the multiplexed `asio_perftest`.
- Add a perftest-style stdout table (`RESULT_FMT`/`RESULT_FMT_LAT` layouts) next to
  the existing JSON (JSON to `--json-out` so `compare_results` is unaffected).
- Add perftest flag-name aliases (`--size`, `--iters`, `--tx-depth`/`--rx-depth`,
  `-c RC`, `-R`, `-D`, `-n`, `-N`) so the same command line drives both tools.
- **Tooling cutover (one coherent step -- the migration checklist):** repoint every
  hard-coded reference, or scenarios/CTest break: `run_scenario`'s `rdma_tool_name()`
  dispatch (`run_scenario.cpp:22-27`); the CMake targets (`CMakeLists.txt:54-57`) +
  alias targets; the CTest invocations by target name (`CMakeLists.txt:64-151`); the
  per-bench operation-validation error strings that name themselves
  (`send_recv.cpp:1541`, `read_write.cpp:782`); and `capability_probe`'s four-boolean
  shape (`capability_probe.cpp:115-120`). `compare_results` is already binary-agnostic
  (reads result JSON by key) so it needs no change here. Recommended cutover: new
  entrypoints canonical, old names kept as aliases for one cycle, then removed.
- **Regression-safety gate (the proof that "numbers unchanged" holds):** before
  refactoring, run the 6 RC cases >=3x via `run_scenario --execute` and archive the
  JSON as the pre-refactor baseline. After each extraction increment, re-run and diff
  `throughput_mib_s` / `latency_p50_us` / `latency_p99_us` against that baseline's
  run-to-run min/median/max envelope, and assert `posted_count == completed_count`.
  Any out-of-envelope shift is a refactor bug, not a new baseline. **Shippable when:**
  all named scenarios run green via the new targets, the diff stays in-envelope, and
  `compare_results` yields an identical table from the new binaries.

### Stage 9b -- Run-structure parity (changes the measured window; resets the baseline) -- DONE

**Implemented (bench-layer only, no rdma-on-asio API change).** A single
`rdma_bench::window_controller` (rdma_bench_common.hpp) drives the warmup ->
timed-window lifecycle for all roles, by op-count (iters mode) or wall deadline
(duration mode), with margin trimming the duration window from both ends. It is
wired into every measure loop -- all six send/recv roles (poll callback-bw,
poll use_future-bw, poll lat, event bw, event lat) and the read_write event/poll
client roles -- with the warmup ramp excluded from the sample arrays by op
SEQUENCE (so a BW pipeline's straddling in-flight ops still pair tposted[i] with
tcompleted[i]). Phase barrier: setup stays the in-band `'R'` ready byte; the
post-window barrier is a 1-byte end-of-stream sentinel for send/recv (the
duration-mode server cannot know the op count) and client disconnect ->
`async_wait_disconnect` for the (passive) read_write server. `--margin-sec` trims
post-hoc in duration mode; `2*margin >= duration` is rejected; the duration-mode
server watchdog is bumped past the window. All verified two-process across
iters / warmup / duration. (`--qp` multi-QP stays DEFERRED -- it needs a connector
interface change; `qps>1` returns not-implemented. See the matrix.)

The original design, for reference: model the measure loop on
`run_iter_bw`/`run_iter_lat` (inline poll on the measuring thread), the phase
barriers on `catch_alarm`, and the setup sync on `ctx_hand_shake`.

- **Warmup loop (new code -- `warmup_iterations` is currently dead):** post-and-
  complete warmup ops excluded from the sample array and byte counters; assert
  measured op count == `completed_count - warmup_iterations`.
- **Per-phase sync barrier:** extend `RDMA_BENCH_READY` (or add a small
  control-message sync) into warmup -> pre-window barrier -> timed window ->
  post-window barrier -> result exchange (server reports to client; the client owns
  the unidirectional-BW number). perftest's reference is a duration state machine
  (`START_STATE/SAMPLE_STATE/STOP_SAMPLE_STATE/END_STATE`).
- **`--margin-sec` (symmetric):** trims the ramp from both ends in duration mode
  (window = duration - 2*margin), mirroring perftest `--margin`.
- **`--qps` -> perftest `--qp` semantics -- DEFERRED (needs a connector interface change):**
  perftest `--qp` is one process, N QPs over **one** connection setup; each QP runs the
  **full** iteration count (no split -- total work is `iters * N`); the client measures the
  aggregate. RDMA-on-Asio's connector is **one cm_id == one QP** (`async_connect(qp, ...)`
  creates the single QP on the cm_id), so N-QP-over-one-connection requires a connector
  interface change -- out of scope this iteration. `asio_perftest` returns a `not_implemented`
  skip for `qps > 1`. (N *independent* connections is a different model --
  `rdma_shared_cq_stress`'s N-independent-listeners -- and stays a stress test, not this
  parity knob.)
- **`compare_results` surfaces the active parity clamps:** add a `constraints` column
  (e.g. `cq-mod=1,inline=0,no-postlist,unidir`) from run metadata so a clamped
  comparison is self-evident in the table, not buried in JSON.

### Stage 10 -- Measurement-fidelity parity (Class D) -- DONE

Implemented: cycle-counter latency stats (`t_min/t_max/t_typical(median)/t_avg/
t_stdev/99%/99.9%` via `fill_latency_cycles`), cycle-counter BW average + peak
(`finish_bw_cycles`; the O(n^2) peak scan is capped so duration-mode's unbounded n
stays bounded -- above the cap peak == average), a latency **histogram**
(fixed log-ish us buckets in the result JSON), and both CPU metrics
(`cpu_util_percent` + `cpu_cycles_per_op`, the latter from process CPU time x the
measured `cpu_mhz`). The original goal, for reference:

- **Latency:** full stats (`t_min/t_max/t_typical(median)/t_avg/t_stdev/p99/
  p99.9`) from a per-iteration delta array, using cycle-counter timestamps
  (perftest's `get_clock`) not `chrono`; add histogram output. The timing-source
  switch (today everything uses `std::chrono::steady_clock`) **will move the latency
  numbers**, so this stage resets the latency baseline -- do not regression-check it
  against the chrono-era 9a numbers.
- **BW peak parity:** perftest's `BW peak` is derived from per-WR cycle-counter
  arrays (`tposted[iters*qps]` / `tcompleted[]`), and `-N/--noPeak` shrinks them to
  one entry (average only). To report a comparable peak, `asio_perftest` must
  record per-op completion timestamps and compute peak the same way; otherwise
  expose only the average and run perftest with `-N` -- do not invent a "peak" by
  another method, the columns must mean the same thing. `--report-gbits` toggles
  Gb/sec vs MiB/sec; support `--run_infinitely` periodic prints. (The duration
  `--margin-sec` ramp exclusion is owned by Stage 9b's run-structure work.)
- **Two distinct CPU metrics (do not conflate):** `cpu_util_percent` mirrors
  perftest `--cpu_util` (a `/proc/stat`-style wall CPU%, the side-by-side number);
  `cpu_cycles_per_op` is an `asio_perftest`-specific abstraction-cost metric
  (cycles per message) with no perftest counterpart. Wire both; report each in its
  own field.

Keep the client-measures-BW and half-RTT conventions.

### Stage 11 -- Library capability features (Class B) -- DEFERRED (out of scope this iteration)

**Not built this iteration.** Each item below needs an rdma-on-asio interface/impl change,
which this iteration excludes (see "This iteration's scope"). `asio_perftest` accepts the
flags for command-line parity but returns a `not_implemented` skip for any non-default
`inline_size` / `cq-mod` / `signaled-every` / `post-list` / `recv-post-list`
(`rdma_bench::not_implemented_reason`, gated in `cli_main`). The designs below are kept as the
reference for a future capability iteration: implement each prerequisite, add its flag, drop
the matching perftest clamp, and re-diff the affected cases. **Sequence by implementation
risk, not comparison value** (the data-plane code makes the risk gradient clear):

1. **`--inline_size` (lowest risk).** Config plumbing already exists
   (`rdma_config_t::max_inline_data_`) but is hard-coded to 0 at QP creation
   (`ibv_service_verbs.hpp`) and never OR'd into `send_flags`. Wire it: set
   `attr.cap.max_inline_data` at create, conditionally OR `IBV_SEND_INLINE` for
   payloads <= the limit (the unused `flag` param in `ibv_ops_verbs.hpp` post_send
   is the natural vehicle). No completion-model change.
2. **`--post_list`/`--recv_post_list` WR batching (medium).** Today `async_send`
   posts exactly one WR per `ibv_post_send`. Add a batch path (a new `async_send_list`
   API or an internal accumulator that chains `wr.next` and flushes on the Nth entry).
   Does not change completion semantics.
3. **`--cq-mod` selective signaling (highest risk -- do LAST, isolated).** Every WR
   is `IBV_SEND_SIGNALED` with `sq_sig_all=1`; the whole completion path resolves one
   CQE to one `rdma_send_op`. Unsignaled WRs get **no** CQE, so this breaks the 1:1
   op-to-CQE contract: it needs a signal counter + batch-complete callback, a new
   `rdma_config_t::signal_interval_` field, and SQ-depth vs CQ-depth accounting. It
   touches the same poller stabilized in Stage 4 -- so gate it on **re-running the
   Stage 4 poller-contract stress test** (`completed_count == posted_count` must still
   hold with unsignaled ops correctly accounted) before it is shippable.

Then `-b` bidirectional and `*_with_imm` (independent of the above ordering).

### Stage 12 -- Breadth and explicit non-goals (Class C) -- non-goals this iteration

UC/UD and atomics (`ib_atomic_*`) need new QP types / opcodes / public API and stay
**non-goals this iteration** -- the public API is RC send/recv/read/write only (no UD address
handle, no atomic ops, hard-coded `IBV_QPT_RC`). The CLI surfaces them as not-implemented:
`--connection` rejects non-RC at parse, and UD/atomics have no operation or entrypoint to
invoke. Pursue them only if a future iteration grows the public API.

### Stage 13 -- Align native ND baseline to perftest

Bring `native_nd_baseline.cpp` onto the same `asio_perftest_core` CLI + output
(Stages 9-10): same perftest-spelled flags, stdout table, latency stats, and
`RDMA_BENCH_READY` handshake (poll-mode only, per the ND baseline rules). Result: a
3-way table -- `baseline=rdma_on_asio` / `perftest` (IBV verbs) / `native_nd` (ND
verbs) -- all sharing topology, size, queue depth, iterations, mode, address.

## Roadmap And Status

**Foundation (done):** capability probe + smoke validation; the benchmark harness
(`rdma_bench_common.hpp`: option parser, result struct + JSON, scenario
loader/validator, clock-overhead calibration, latency-sample collector, env
capture, server-ready handshake, raw-output archiver); send/recv and read/write
bw+lat in both event and poll mode (`rdma_send_recv_bench`,
`rdma_read_write_bench`; poll uses standalone CQ + `as_tuple(use_future)`,
`use_awaitable` latency is a capability skip; read/write exchanges
`rdma_remote_addr_t` via private data and validates payloads); the benchmark tools
(`capability_probe`, `perftest_commands`, `run_scenario` with `--execute` for
single-host same-process, `compare_results`, `parse_perftest`); the concurrency
stress + soak tests; and the native ND baseline (above).

**Shared-CQ poller contract (Stage 4 stress -- the standing CLAUDE.md TODO).**
`rdma_shared_cq_stress` must exercise and assert: several `run()` threads +
several submitters over multiple QPs sharing one io_context CQ; the poller starts
exactly once, lazily, at the first event-mode `bind(io)` and re-arms (no second
poller as QPs/threads grow); `completed_count == posted_count` (minus documented
cancellations -- the main race signal); `run()` does not return on idle once the
poller is outstanding, shutdown via `io.stop()` exits cleanly with all threads
joined; poll-only / control-plane-only io_contexts still return on idle.

**Connect/disconnect soak (Stage 5).** `rdma_connect_disconnect_soak`: repeated
connect/accept/disconnect, cancellation racing connect, disconnect racing
establishment, wait-disconnect re-arm; dimensions = iterations, concurrent
connectors, event/poll setup, optional randomized delays. Broad race discovery;
specific-bug regressions stay in `unit_test_plan.md`.

**Reporting (Stage 6).** Raw JSON under an output dir with git/backend/build/
compiler/OS/CPU/NIC/command-line; perftest command lines in the same dir;
comparison tables (RDMA-on-Asio vs perftest, and vs native ND); raw stdout/stderr
preserved as source of truth. Performance numbers are not a CI pass/fail threshold
until a stable dedicated runner exists -- use only sanity checks (nonzero
throughput, all iterations completed, p99 recorded).

**Next: Stage 8 -- IBV vs perftest execution.** The design above exists; what is
missing to run it:

1. **Managed-build wiring** -- `RDMA_BUILD_PERFTEST`/`managed` mode is an
   unimplemented option (no submodule, no `ExternalProject`).
2. **Mode resolution + skip** -- a `perftest_resolve` step turning mode + dir
   options into validated `ib_*` paths (or a clean "unavailable" skip), capturing
   the perftest version into metadata.
3. **Orchestration** -- a driver that, per matrix case, runs the lib bench
   (`lib_<case>.json`), asks `perftest_commands` for the lines, runs perftest
   (server backgrounded, client foreground), feeds stdout to `parse_perftest`
   (`perftest_<case>.json`), then collects all through `compare_results`.
4. **No executed IBV-vs-perftest numbers exist yet.**

Phases: **0** (zero new code, done for bandwidth) `apt install perftest`, manually
drive cases through `perftest_commands -> perftest -> parse_perftest ->
compare_results`; **1 (done)** `perftest_resolve` + skip + version, implemented as
logic inside the shell driver (`tools/run_comparison.sh`), not a new binary;
**2 (done)** the driver runs all cases single-host two-process (incl. latency,
event) over the `scenarios/comparison/*.json` matrix, with the known gaps handled
inline (perftest write has no `--events` -> skip that side; poll read/write lib
bench forced to `use_future`); **3 (skipped)** managed submodule + `ExternalProject`
-- intentionally not done, use system perftest in PATH or `--perftest-bin-dir`;
**4 (done)** publish an "IBV vs perftest (RoCE)" results section -- README's Linux
table now carries the full bw+lat matrix (send/recv, write, read x event/poll) from
`run_comparison.sh` over `scenarios/comparison/*.json`, on the Stage 9b/10 measure
window with the cq-mod-1/inline-0/qp-1 parity clamp surfaced; **5** moved to
**Stage 14** (two-host topology + CI matrix), deferred to last.

**Remaining gaps across the suite:** full two-process orchestration with
server-ready log parsing; deeper perftest parser coverage across versions; CPU
fields beyond process utilization (`cpu_cycles_per_op`, context switches); deep
NIC driver/firmware/MTU/link-speed capture.

## Acceptance Criteria

- Unit/correctness tests stay separate; stress/performance/latency never run in
  default CTest; benchmark tools run as explicit client/server programs.
- Scenario files drive RDMA-on-Asio, perftest, and native-ND runs without source
  changes; measurement semantics are defined per operation before comparing.
- Scenario knobs are validated against real library capability: this iteration,
  inline / selective-signaling / WR-batching are **skipped via a `not_implemented`
  gate** (not matched by constraining perftest), so a comparison never reports a
  number for per-message work the library does not actually perform.
- Token type is recorded; poll mode uses a non-io_context token; CPU-efficiency
  metrics are captured so abstraction cost is assessable at line rate.
- The Stage 4 stress test asserts the shared-CQ poller contract (single lazy
  poller, consistent completion count, clean `io.stop()`).
- Capability probes distinguish unsupported scenarios from real failures; result
  JSON is schema-versioned with explicit units; runner timeouts + raw-log
  preservation make hangs diagnosable.
- IBV has a clear perftest comparison path (send/read/write); perftest stays an
  external, unlinked, non-vendored baseline.
- ND has a clear native direct-ND comparison path; the plan keeps RDMA-on-Asio ND
  results distinct from native ND baseline results.
- Single-host results (same-process and two-process) are labeled
  development/regression signals and are the validated comparison path; real
  two-box (multi-host) results carry topology/host/NIC/command-line metadata and
  are deferred to Stage 14.

## Stage 14 -- Two-host (multi-box) validation (deferred to last)

Principle: every case (send/recv, read, write x {bw, lat} x {event, poll}) is
validated single-host two-process now via `tools/run_comparison.sh` over the
`scenarios/comparison/*.json` matrix (Stage 8 Phases 1-2). Real two-box runs are
the final stage and gate nothing before them.

Collected here from earlier stages:
- Stage 8 Phase 5: `two_host_direct` / `two_host_switch` topology runs and the CI
  matrix that exercises them.
- The scenario files that declare `topology: two_host_direct` (`baseline.json`,
  `sweep.json`) are Stage 14 inputs; their single-host two-process counterparts
  under `scenarios/comparison/` are the Stage 8 inputs.
- The multi-host acceptance line (topology/host/NIC/command-line metadata on real
  two-box results).

Scope when undertaken: parameterize `run_comparison.sh` with distinct
`--local-addr` (server box) and `--peer-addr` (client box) across two hosts, add a
host-pairing/launch mechanism (ssh or CI runners), and wire a CI matrix. No
library changes -- driver and scenario reuse only.
