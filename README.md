# rdma-on-asio

A cross-platform **RDMA abstraction layer** that integrates with
[Asio](https://think-async.com/Asio/)'s asynchronous model and C++20 coroutines (`co_await`).

Write your RDMA application **once** against a single portable API; the same source compiles
and runs on:

- **Linux:** libibverbs + rdma_cm (`ibv` backend, epoll-based)
- **Windows:** NetworkDirect (`nd` backend, IOCP-based)

## Highlights

- **One portable API.** `#include "rdma/rdma.hpp"` and use the backend-agnostic `rdma_*`
  types + the `tcp` port space. A compile-time `#if` selects the backend.
- **Native Asio integration.** Every operation is an `async_*` initiation that works with
  `co_await`, callbacks, futures, `asio::as_tuple`, etc.
- **Connection management like TCP.** `connector` / `listener` mirror Asio's
  acceptor/socket model; on Linux the `rdma_resolve_addr`/`resolve_route` handshake is hidden
  inside `async_connect`.
- **Two completion modes.** Event-driven (shared CQ bridged into the Asio reactor/IOCP) or
  manual poll-mode (`completion_queue::poll()`) with an **io_context-free data plane** -- bind the
  queue pair to a standalone CQ and complete data-plane ops on your own polling thread.

## Quick start

The data plane (`async_send` / `async_recv` / `async_read` / `async_write`) can deliver its
completions two ways. Both examples below share the same setup: discover a device via
`rdma_device_manager_t`, hand it to `use_device(io, dev)` (which installs the per-`io_context`
completion service and is where the operating config is set), then run `connector` / `listener`
on that `io_context`. One device can be shared across several `io_context`s -- call
`use_device` with the same `rdma_device_ptr` on each.

### Event-driven mode (default)

`use_device()` installs a shared completion queue that is bridged into Asio's reactor (IOCP on
Windows, comp_channel + epoll on Linux). Verbs-op completions are posted to the `io_context`,
so they compose with `co_await`, callbacks, futures, `asio::as_tuple`, etc. -- you just drive
`io.run()`.

```cpp
#include "rdma/rdma.hpp"
#include "asio/io_context.hpp"
#include "asio/co_spawn.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/as_tuple.hpp"

using namespace asio::rdma;
constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> client(asio::io_context& io, rdma_device_ptr dev,
                             const std::string& host, uint16_t port) {
  rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());                                     // create the cm_id (client)
  rdma_queue_pair     qp(io);                               // event-driven: uses the shared CQ

  std::string hello = "client-hello";
  co_await conn.async_connect(qp, tcp::endpoint(asio::ip::make_address(host), port),
                              asio::buffer(hello), nothrow); // send request pd; QP created here
  // (to also receive the server's reply pd, pass a mutable buffer:
  //  auto [ec, reply_len] = co_await conn.async_connect(qp, ep, asio::buffer(hello),
  //                                                     asio::buffer(reply), nothrow);)

  std::array<char, 4096> buf{};
  rdma_memory_region mr(dev, buf.data(), buf.size());       // register memory
  std::memcpy(buf.data(), "hello", 5);

  co_await qp.async_send(mr.cslice(0, 5), nothrow);         // RDMA SEND
  auto [ec, n] = co_await qp.async_recv(mr.slice(0, buf.size()), nothrow);  // RECV
  conn.disconnect();                                        // synchronous teardown
}

int main() {
  asio::io_context io;
  auto dev = rdma_device_manager_t::instance()
                 .get_first_available_device(tcp::v4(), {});   // discover a device
  use_device(io, dev);                   // install this device's shared CQ on io
  asio::co_spawn(io, client(io, dev, "10.0.0.1", 5000), asio::detached);
  io.run();                              // pumps both the CM handshake and verbs completions
}
```

On the server side, the listener hands you a ready-to-accept `connector` (carrying the peer's
private data) and you accept onto a fresh queue pair:

```cpp
rdma_listener<tcp> listener(io);
listener.open(tcp::v4());
listener.bind(port);
listener.listen();

std::array<char, 256> request{};                                    // receives the client's request pd
auto [ec, conn, request_len] =
    co_await listener.async_get_connection(asio::buffer(request), nothrow);  // -> a connector

rdma_queue_pair qp(io);
co_await conn.async_accept(qp, nothrow);                            // QP created during accept (no reply pd)
// (to send reply pd: co_await conn.async_accept(qp, asio::buffer(reply), nothrow);)
// ... qp.async_recv / qp.async_send ...
```

### Poll mode (manual completion queue)

Create a standalone `rdma_completion_queue`, bind the queue pair to it with the
**`(cq)` constructor -- no `io_context`** -- and reap data-plane completions yourself with
`cq.poll()` / `cq.poll_one()`. With a non-`io_context`-bound completion token (a callback or
`use_future`, *not* `use_awaitable`), the handler fires **inline on the thread that calls
`poll()`**, so the data plane never touches an `io_context`. The control-plane handshake
(`connect` / `accept` / `disconnect`) still runs on an `io_context`; only the data plane is
io_context-free. A common pattern is a dedicated thread spinning `cq.poll()` while the data
path blocks on `use_future`:

```cpp
#include "rdma/rdma.hpp"
#include "asio/io_context.hpp"
#include "asio/use_future.hpp"
#include "asio/as_tuple.hpp"
#include <thread>

using namespace asio::rdma;
constexpr auto use_fut = asio::as_tuple(asio::use_future);  // no exceptions; completes in poll()

int main() {
  asio::io_context io;
  auto dev = rdma_device_manager_t::instance().get_first_available_device(tcp::v4(), {});
  use_device(io, dev);                 // required even in poll mode (backs the QP)

  rdma_completion_queue cq(dev);       // standalone CQ (no comp_channel), holds the device
  rdma_connector<tcp>   conn(io);
  conn.open(tcp::v4());
  rdma_queue_pair       qp(cq);        // poll-mode QP -- bound to cq, NO io_context

  // control plane: connect on the io_context (QP is created on the connection)
  std::string hello = "client-hello";
  conn.async_connect(qp, tcp::endpoint(asio::ip::make_address("10.0.0.1"), 5000),
                     asio::buffer(hello), [](asio::error_code /*ec*/) {});
  io.run();                            // pump the rdma_cm handshake to completion

  // data plane: spin a poll thread; ops complete inline there, no io.run()
  std::atomic<bool> stop{false};
  std::thread poller([&] { while (!stop) cq.poll(); });

  std::array<char, 4096> buf{};
  rdma_memory_region mr(dev, buf.data(), buf.size());
  std::memcpy(buf.data(), "hello", 5);
  auto [ec, n] = qp.async_send(mr.cslice(0, 5), use_fut).get();  // blocks; poll thread completes it

  stop = true; poller.join();
}
```

A complete, runnable echo client/server is in
[`tests/rdma/test_rdma_echo.cpp`](tests/rdma/test_rdma_echo.cpp).

## Public API

Include `rdma/rdma.hpp`. All names live in `namespace asio::rdma`.

| Type / call | Purpose |
|-------------|---------|
| `rdma_device_manager_t::instance()` | Process-wide device registry; `get_first_available_device(port_space, config)` -- `rdma_device_ptr` (first device whose caps satisfy the non-zero `config` constraints) |
| `use_device(io, device, config = {})` | Install the per-`io_context` completion service for `device`; sets the operating config. Returns `void`; share one `device` across multiple `io_context`s by calling it on each |
| `rdma_device_ptr` | Handle to a device (from `get_first_available_device`) |
| `rdma_memory_region` | RAII memory region; `slice()` / `cslice()` (or `asio::rdma::buffer(mr[, off, n])`) produce value-semantic `const_buffer` / `mutable_buffer` (`{addr, len, lkey}`); `remote_addr(off, n)` -- `{addr, rkey}` to advertise a sub-range to a peer |
| `rdma_connector<tcp>` | Control plane: `open(port_space)` / `async_connect(qp, ep, request, reply)` -- `(ec, reply_len)` (or no-reply `async_connect(qp, ep, request)` -- `(ec)`) / `async_accept(qp, reply)` -- `(ec)` (or no-reply `async_accept(qp)`) / `disconnect()` (sync) |
| `rdma_listener<tcp>` | Server: `open(port_space)` / `bind(port)` / `listen` / `async_get_connection(request)` -- `(ec, connector, request_len)` (recv the client's request pd) / `cancel()` (abort pending get; listener stays reusable) |
| `rdma_queue_pair` | Data plane: `async_send` / `async_recv` / `async_read` / `async_write`. Binds to a completion mechanism: `rdma_queue_pair(io)` = event-driven; `rdma_queue_pair(cq)` = poll-mode (io_context-free data plane). `bind()` (deferred) / `is_bound()` / `bound_type()` -- `completion_mode` |
| `rdma_completion_queue` | Standalone poll-mode CQ; `poll()` / `poll_one()` |
| `tcp` | Port space: `tcp::endpoint`, `tcp::resolver`, and `tcp::{connector,listener}` (the data-plane `queue_pair` is port-space-agnostic -- use `rdma_queue_pair`) |
| `rdma_config_t` | Capacities (CQ depth, WR/SGE limits, inline data, read limits); `0` = auto-derive from device caps |

`rdma_connector`/`rdma_listener` are templated on the port space (they carry the endpoint
type); `rdma_queue_pair` and friends are not (the data plane is port-space-agnostic).

### Scatter/gather

A buffer element (`asio::rdma::const_buffer` / `mutable_buffer`) is value-semantic
(`{addr, length, lkey}`, cross-platform -- not per-backend), so any standard buffer sequence is
a multi-segment SGL: pass a `std::vector` / `std::array` of them (each segment may come from a
different MR / lkey).

```cpp
std::vector<rdma_const_buffer> gather{ mr_a.cslice(0, n1), mr_b.cslice(0, n2) };
co_await qp.async_send(gather, token);     // one SEND, two SGEs spanning two MRs

std::vector<rdma_mutable_buffer> scatter{ rdma::buffer(mr_r1, 0, n1),
                                          rdma::buffer(mr_r2, 0, n2) };
co_await qp.async_recv(scatter, token);    // scatter the recv across two MRs
```

A single buffer is a 1-element sequence, so the plain `qp.async_send(mr.cslice(...))` spelling is
unchanged. Posting more segments than the device's `max_send_sge` / `max_recv_sge` is rejected
before it reaches the HCA with `rdma_errc::too_many_sge` (a clean library error, not a raw
HW failure). The local SGE only ever uses `lkey`; for RDMA read/write the **remote** target is a
separate `rdma_remote_addr_t` carrying the peer's `rkey` (from its `mr.remote_addr(off, n)`).

### Private data (connect/accept)

The CM handshake exchanges a small private-data payload each direction. The API is **symmetric**:
you **send** with a `const_buffer` arg and **receive** into a `mutable_buffer` out-param (filled on
completion), with a `std::size_t` telling you how many bytes were written:

```cpp
// client: send request, receive the server's reply
auto [ec, reply_len] = co_await conn.async_connect(qp, ep, asio::buffer(req),
                                                   asio::buffer(reply), token);
// server: receive the client's request, then send a reply
auto [ec, conn, request_len] = co_await lis.async_get_connection(asio::buffer(req), token);
co_await conn.async_accept(qp, asio::buffer(reply), token);
```

Notes:
- **Don't need the received pd?** Use the convenience overloads: `async_connect(qp, ep, request, token)`
  (completion `void(ec)`, no `reply_len`) and `async_accept(qp, token)` (send no reply).
- Each direction is independent -- one side may send while the other sends nothing (pass `{}`).
- **The received length is rdma_cm's transport-padded length, not the sender's exact length**
  (the receiver gets a fixed zero-filled field). If you need the exact length, frame it yourself
  (e.g. a length prefix).
- Outgoing private data is capped at 255 bytes (`rdma_conn_param.private_data_len` is a `uint8_t`);
  larger is rejected with `rdma_errc::private_data_too_large`. The outgoing buffer need not
  outlive the call (it is copied at initiation); the receive buffer must stay valid until completion.

## Performance Snapshot

Benchmark history is tracked in
[`docs/rdma_stress_performance_results.md`](docs/rdma_stress_performance_results.md), with the
read/write optimization follow-up in
[`docs/rdma_read_write_performance_optimization_plan.md`](docs/rdma_read_write_performance_optimization_plan.md).

RDMA-on-Asio is benchmarked with `asio_perftest` (`tests/benchmark/`), whose CLI and
verb-driving loop deliberately mirror [linux-rdma/perftest](https://github.com/linux-rdma/perftest)
so the two can be compared directly. The two backends run on different hardware and are reported
separately: **Linux** (`ibv`, compared against perftest) and **Windows** (`nd`, compared against a
native NetworkDirect baseline). All numbers are local engineering / regression signals on
single-host loopback, not cross-machine line-rate claims.

### Linux (`ibv` backend) -- RDMA-on-Asio vs perftest

Collected 2026-06-18 via `tools/run_comparison.sh` (the Stage 8 driver: per case it runs the lib
bench and perftest two-process, parses both, and diffs them), single host, loopback over RoCE.
Values are the median of 3 runs (single-host loopback has real run-to-run variance).

| Item | Value |
|---|---|
| CPU | AMD EPYC 7542 (16 vCPU exposed) |
| RDMA NIC | Mellanox ConnectX `mlx5_0`, fw 22.37.1014 |
| Link | 100 Gbit/s (4X EDR), RoCE / Ethernet link layer |
| OS/backend | Linux 6.8 + libibverbs + rdma_cm (`ibv`) |
| Topology | Single host, two-process loopback (`--server` / `--client`) |
| Baseline | linux-rdma/perftest 6.20 (`ib_{send,write,read}_{bw,lat}`) |
| Build | Release |

Method: bandwidth is a 64 B / 4 KiB / 64 KiB / 128 KiB sweep at queue/tx depth 64, 20000 iters
per point; latency is 4 KiB, one outstanding, 2000 iters. The measured window is Stage 9b's
warmup-capable, cycle-counter window; latency uses Stage 10's perftest-faithful percentiles.
**Parity clamp (`constraints` column in the driver output): `cq-mod=1, inline=0, post-list=1,
qp=1`** -- the asio side has no selective signaling / inline / WR batching / multi-QP this iteration
(Stage 11/12 deferred), so perftest is constrained to match (one signaled, non-inline WR per
message, single QP). Mode mapping: asio `--mode poll` (busy-poll) <-> perftest default busy-poll;
asio `--mode event` (CQ comp-channel on epoll) <-> perftest `--events`. Token note: all poll rows
(send/recv **and** read/write) use an inline `callback` with rolling queue depth -- each completion
reposts its own slot, so a single thread busy-polls the CQ exactly like perftest.
`as_tuple(use_future)` (promise/future + heap + locking + a background CQ-spinner thread) is
retained only as a token-overhead diagnostic, not the baseline. All event rows use `callback`.

Send/recv bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.884 | 31.113 | 93.200 | 93.450 | 93.450 |
| RDMA-on-Asio / poll callback | 1.136 | 74.078 | 93.445 | 93.806 | 93.806 |
| perftest / event | 0.734 | 24.046 | 90.161 | 90.208 | 90.208 |
| perftest / poll | 1.166 | 85.775 | 90.202 | 90.343 | 90.343 |

RDMA write bandwidth (Gbit/s) (perftest has no event mode for the WRITE verb):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 1.070 | 80.070 | 94.109 | 94.172 | 94.172 |
| RDMA-on-Asio / poll callback | 1.146 | 86.425 | 94.356 | 94.105 | 94.356 |
| perftest / poll | 1.158 | 83.138 | 90.115 | 90.210 | 90.210 |

RDMA read bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 1.100 | 65.388 | 62.252 | 61.239 | 65.388 |
| RDMA-on-Asio / poll callback | 1.152 | 65.470 | 62.105 | 61.230 | 65.470 |
| perftest / event | 1.158 | 72.212 | 70.817 | 70.863 | 72.212 |
| perftest / poll | 1.164 | 72.032 | 70.809 | 70.833 | 72.032 |

Latency (us, 4 KiB, event mode; send/write report half-RTT, read the full round trip):

| op | RDMA-on-Asio p50 / p99 | perftest p50 |
|---|---:|---:|
| send/recv | 20.9 / 29.1 | 23.8 |
| read | 17.2 / 27.2 | 29.1 |
| write | 8.1 / 12.3 | n/a |

(perftest emits "Events feature not available on WRITE verb", so the write rows have no perftest
event/latency baseline.)

Observations:

- **Large messages saturate the fabric**: at 64 KiB+ send/recv and write reach ~93-94 Gbit/s on
  RDMA-on-Asio, a hair above perftest's ~90 -- both sit near the ~100G line-rate ceiling and the
  abstraction adds nothing measurable here.
- **4 KiB is the scheduling-sensitive point**: poll write leads perftest (86.4 vs 83.1) and event
  send/recv leads perftest (31.1 vs 24.0), while poll send/recv trails perftest (74.1 vs 85.8). The
  mid-size point is where per-op overhead matters most and run-to-run variance is largest.
- **Poll read/write no longer collapse**: callback rolling-QD puts 4 KiB read/write at 65.5 / 86.4
  Gbit/s, erasing the old `use_future` figures (38.8 / 9.3) -- that gap was promise/future + a
  background spinner thread, not a fabric limit.
- **read tops out lower than send/write on both stacks** (~62 asio vs ~71 perftest at 64 KiB+);
  perftest read sustains ~10-15% higher across all sizes. RDMA read is bounded by responder
  resources + the request round trip, so this is a read-path gap to profile, not a wrapper cost.
- **64 B is message-rate bound** (~0.7-1.2 Gbit/s); poll edges event, as expected with no
  comp-channel wakeup per op.
- **Latency is competitive**: event-mode p50 (8-21 us) is at or below perftest's on send/recv and
  read -- the abstraction adds little on the one-deep path.
- Numbers are the median of 3 runs on single-host loopback (client, server, and perftest contend
  for the same cores), so treat them as regression signals, not line-rate claims.

### Windows (`nd` backend) -- RDMA-on-Asio vs native NetworkDirect

Collected 2026-06-18 with `tests/benchmark/tools/run_nd_comparison.ps1`, which runs
`asio_perftest` and `nd_perftest` over the same scenario set. The sweep uses single-host
same-process loopback, 64 outstanding WRs, 20000 measured iterations, and 128 warmup iterations.
The poll rows below use callback rolling-QD completion for RDMA-on-Asio; `use_future` is kept only
as a token-overhead diagnostic. These numbers are local engineering / regression signals, not final
cross-machine line-rate claims.

Test environment:

| Item | Value |
|---|---|
| CPU | AMD EPYC-Rome Processor, 32 cores / 32 logical processors, 2.895 GHz max clock |
| RDMA NIC | ConnectX Family mlx5Gen Virtual Function |
| RDMA enabled | Yes |
| Reported link speed | 100 Gbit/s |
| Total tested RDMA link bandwidth | 100 Gbit/s |
| OS/backend | Windows + NetworkDirect (`nd`) |
| Topology | Single host, same-process loopback |
| Build | Release |
| Comparison baseline | direct ND2 API via `nd_perftest` (`baseline=native_nd`, poll mode only) |

Native ND is intentionally poll-only. Event-mode rows therefore measure RDMA-on-Asio scheduler
integration only; the native event baseline returns an explicit skip.

Send/recv bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.406 | 27.513 | 88.682 | 90.857 | 90.857 |
| RDMA-on-Asio / poll callback | 0.758 | 51.453 | 91.473 | 91.722 | 91.722 |
| Native ND / poll | 0.868 | 48.654 | 89.535 | 90.679 | 90.679 |

RDMA write bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.507 | 38.184 | 89.206 | 84.474 | 89.206 |
| RDMA-on-Asio / poll callback | 0.763 | 54.453 | 90.982 | 91.769 | 91.769 |
| Native ND / poll | 0.862 | 62.715 | 90.831 | 91.128 | 91.128 |

RDMA read bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.505 | 36.352 | 61.953 | 61.025 | 61.953 |
| RDMA-on-Asio / poll callback | 0.756 | 64.376 | 61.956 | 61.033 | 64.376 |
| Native ND / poll | 0.842 | 47.729 | 62.130 | 61.182 | 62.130 |

Observations:

- Large-message send/recv and write remain close to the 100G link on both RDMA-on-Asio and native
  ND poll paths.
- The 2026-06-18 read sweep supersedes the older 8-9 Gbit/s single-host read snapshot: current
  RDMA read reaches about 61-62 Gbit/s on both RDMA-on-Asio and native ND for 64 KiB+ messages.
- RDMA-on-Asio poll read/write now use callback rolling-QD for the fair baseline. The 4 KiB
  write path improved from the old `use_future` snapshot (15.295 Gbit/s) to 54.453 Gbit/s, and
  the read path is now in the same range as native ND on this single-host loopback run.

Next performance work:

- Add `--validate full|sample|none` so benchmark validation cost is visible and configurable.
- Continue profiling the SGE fast path on Linux/IBV and larger multi-SGE workloads; the common
  single-buffer path now uses stack native SGEs and the small-SGL path avoids TLS.
- Measure custom associated allocators and operation recycling for small-message hot paths while
  preserving standard Asio async completion semantics.
- Continue event-mode tuning by measuring CQ drain batch sizes and re-arm frequency.
- Extend the Linux and Windows comparisons to real multi-host runs when matching hardware on both
  ends is available.

## Requirements

- A C++20 compiler (coroutines, concepts).
- CMake 3.20+.
- Submodules: `third_party/asio`, `third_party/networkdirect` -- `git submodule update --init`.
- **Linux:** rdma-core dev packages -- `sudo apt install libibverbs-dev librdmacm-dev` (or `dnf install rdma-core-devel`).
- **Windows:** NetworkDirect SDK (vendored under `third_party/networkdirect`).

## Build

```sh
git submodule update --init --recursive
cmake -B build -DRDMA_BACKEND=ibv      # Linux (default); use -DRDMA_BACKEND=nd on Windows
cmake --build build
```

`RDMA_BACKEND` defaults to `ibv` on Linux and `nd` on Windows.

## Layout

```
include/
  rdma/         public, backend-agnostic API (rdma.hpp, rdma_types.hpp, rdma_commons.hpp,
                rdma_buffer.hpp, tcp.hpp, detail/rdma_*op*)
    ibv/        Linux libibverbs + rdma_cm backend
    nd/         Windows NetworkDirect backend
tests/
  unit/         Asio-style deterministic unit tests
  rdma/         cross-platform tests (rdma_* API), built for either backend
  ibv/  nd/     backend-specific tests
```

## Status

- **ibv (Linux):** implemented and verified end-to-end over RoCE (device discovery,
  connect/accept/listen, send/recv/read/write, MR, dual completion modes, echo).
- **nd (Windows):** implemented and verified on Windows against the same public surface.

## Cancellation

**Control plane -- object-level *and* per-operation (ibv: implemented + RoCE-verified; nd:
implemented + Windows-verified).**

- *Object-level teardown.* `connector::disconnect()` is one thread-safe, state-adaptive call: it
  aborts an in-flight `async_connect`/`async_accept`, **or** tears down an established connection
  (pending data-plane ops then flush to `operation_aborted`). Because an rdma_cm `cm_id` is
  single-use, a connector is **terminal** after disconnect -- a reused `async_connect` is rejected
  up front with `rdma_errc::connector_terminal`; create a fresh connector. `listener::cancel()` aborts a
  pending `async_get_connection` and leaves the listener in `LISTEN` (reusable, like
  `acceptor::cancel()`).
- *Per-operation (Asio cancellation slots).* `async_connect`, `async_accept`,
  `async_get_connection`, and `async_wait_disconnect` honor a `cancellation_slot`, so
  `cancel_after`, `co_spawn` cancellation, `awaitable_operators` `||`, and `parallel_group` act on
  one specific control-plane op. Cancelling a connect/accept makes the connector terminal;
  cancelling `async_wait_disconnect` only stops the watcher -- the connection stays usable.

**Data plane -- no per-operation cancellation, by design.** Once a send/recv/read/write work
request is posted, it belongs to the HCA: its buffers stay pinned until the matching completion,
and standard verbs offers no `ibv_cancel_wr` to retract a single in-flight WR. The only standard
lever is a *whole-queue* state change -- moving the QP to ERROR (which flushes **all** outstanding
WRs as `operation_aborted`) or destroying it. There is one vendor-private exception,
`mlx5dv_qp_cancel_posted_send_wrs()` (mlx5 DirectVerbs), which cancels posted *send* WRs by
`wr_id` -- but it is narrow and heavyweight: the QP must first be drained into the SQD state, it
must have been created with `MLX5DV_QP_CREATE_SIG_PIPELINING` (the API exists for the
signature-pipelining offload, not general use), it needs a DEVX context, and it covers sends only.
Being a Mellanox/NVIDIA-private API with those constraints, it is not a portable, general per-WR
cancel. **So this layer does not implement data-plane single-op cancellation** -- to abort
in-flight data-plane ops, tear the connection down with `disconnect()`, whose QP->ERROR flush
completes them as `operation_aborted`.

## TODO

- **Performance follow-up** -- continue validation-mode controls, allocator measurements, event
  scheduler tuning, and multi-host `perftest` comparison. This work is tracked in
  [`docs/rdma_stress_performance_plan.md`](docs/rdma_stress_performance_plan.md) and
  [`docs/rdma_read_write_performance_optimization_plan.md`](docs/rdma_read_write_performance_optimization_plan.md).

**Done:** a deterministic, no-hardware unit-test suite (Asio-style harness,
`RDMA_BUILD_UNIT_TESTS`, default `ctest`) plus opt-in hardware integration/regression
tests (`RDMA_ENABLE_HARDWARE_TESTS` + `RDMA_TEST_ADDR`) now cover the public API,
buffer/op/config/error logic, and service-state guards -- see
[`docs/unit_test_plan.md`](docs/unit_test_plan.md).

See [`CLAUDE.md`](CLAUDE.md) for the detailed architecture and design notes.
