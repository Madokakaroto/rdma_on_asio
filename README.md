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

Collected 2026-06-16, single host, two-process loopback over RoCE.

| Item | Value |
|---|---|
| CPU | AMD EPYC 7542 (16 vCPU exposed) |
| RDMA NIC | Mellanox ConnectX `mlx5_0`, fw 22.37.1014 |
| Link | 100 Gbit/s (4X EDR), RoCE / Ethernet link layer |
| OS/backend | Linux 6.8 + libibverbs + rdma_cm (`ibv`) |
| Topology | Single host, two-process loopback (`--server` / `--client`) |
| Baseline | linux-rdma/perftest (`ib_send_bw` / `ib_write_bw` / `ib_read_bw`) |
| Build | Release |

Method: `-n 20000`, queue/tx depth 128, `--report_gbits`, sizes 64 B / 4 KiB / 64 KiB / 128 KiB.
Mode mapping: asio `--mode poll` (busy-poll, inline completion) corresponds to perftest's default
busy-poll; asio `--mode event` (CQ completion channel on epoll) corresponds to perftest `--events`.
Token note: asio send/recv poll uses an inline `callback` (no io_context on the data path), while
asio read/write poll currently uses `as_tuple(use_future)` (promise/future + heap + locking) -- a
poll+callback read/write path is still TODO, so the read/write poll rows carry that overhead at
small/medium sizes. All event-mode rows use `callback`.

Send/recv bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB |
|---|---:|---:|---:|---:|
| RDMA-on-Asio / poll (callback) | 1.13 | 71.78 | 91.41 | 91.59 |
| RDMA-on-Asio / event (callback) | 1.15 | 68.27 | 93.75 | 92.17 |
| perftest / busy-poll | 1.29 | 91.81 | 96.41 | 96.32 |
| perftest / events | 1.29 | 65.26 | 96.12 | 96.27 |

RDMA write bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB |
|---|---:|---:|---:|---:|
| RDMA-on-Asio / poll (use_future) | 0.16 | 55.03 | 91.18 | 91.99 |
| RDMA-on-Asio / event (callback) | 1.09 | 81.55 | 90.87 | 89.64 |
| perftest / busy-poll | 1.42 | 92.04 | 94.34 | 96.02 |
| perftest / events | n/a | n/a | n/a | n/a |

perftest does not implement an events mode for the WRITE verb ("Events feature not available on
WRITE verb"), so those cells are n/a.

RDMA read bandwidth (Gbit/s):

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB |
|---|---:|---:|---:|---:|
| RDMA-on-Asio / poll (use_future) | 0.24 | 53.20 | 62.11 | 61.13 |
| RDMA-on-Asio / event (callback) | 1.08 | 65.24 | 62.25 | 61.04 |
| perftest / busy-poll | 1.40 | 75.13 | 73.26 | 73.56 |
| perftest / events | 1.35 | 72.24 | 73.15 | 73.48 |

Observations:

- Large messages (64-128 KiB): send/recv and write reach near line rate on both paths
  (RDMA-on-Asio 90-94 Gbit/s vs perftest 94-96 Gbit/s).
- RDMA read plateaus well below write/send on both paths (perftest ~73, RDMA-on-Asio ~62 Gbit/s) --
  read throughput is bounded by `initiator_depth` / round-trip latency here, not the wire.
- At 4 KiB the perftest busy-poll path leads (send/recv 92 vs 72 Gbit/s); the gap is wrapper cost
  plus, for read/write poll, the `use_future` overhead. asio event-mode write at 4 KiB reaches
  81 Gbit/s.
- 64 B is message-rate bound (sub-1.5 Gbit/s) rather than bandwidth bound on every path; the two
  stay in the same order of magnitude.

### Windows (`nd` backend)

The 2026-06-15 measurements were collected on a single Windows host using the NetworkDirect
backend and same-process loopback. They are useful as local engineering and regression signals,
not as final cross-machine line-rate claims.

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
| Build | Release, plus RelWithDebInfo for CPU sampling/flame graphs |

Send/recv bandwidth summary from the stable schedule comparison:

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.561 Gbit/s | 43.840 Gbit/s | 90.801 Gbit/s | 92.797 Gbit/s | 92.797 Gbit/s |
| RDMA-on-Asio / poll callback | 0.950 Gbit/s | 50.873 Gbit/s | 93.209 Gbit/s | 91.979 Gbit/s | 93.209 Gbit/s |
| Native ND / poll | 0.975 Gbit/s | 57.003 Gbit/s | 91.201 Gbit/s | 90.101 Gbit/s | 91.201 Gbit/s |

The dedicated send/recv poll/callback run also reached `93.172 Gbit/s` at `128 KiB`, while the
native ND direct run reached `92.979 Gbit/s` at the same size. In this single-host setup,
large-message send/recv is therefore close to the reported 100G link rate on both the portable
RDMA-on-Asio path and the native ND poll baseline.

RDMA write bandwidth summary:

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.635 Gbit/s | 44.024 Gbit/s | 91.681 Gbit/s | 91.905 Gbit/s | 91.905 Gbit/s |
| RDMA-on-Asio / poll `use_future` | 0.209 Gbit/s | 13.720 Gbit/s | 92.170 Gbit/s | 92.946 Gbit/s | 92.946 Gbit/s |
| Native ND / poll | 1.086 Gbit/s | 71.165 Gbit/s | 89.088 Gbit/s | 89.470 Gbit/s | 89.470 Gbit/s |

The current RDMA-on-Asio poll read/write benchmark uses `as_tuple(use_future)`, so its small and
medium-message numbers include promise/future allocation, heap traffic, and locking. A fairer
poll/callback read/write benchmark is planned before drawing final wrapper-overhead conclusions.

RDMA read bandwidth summary:

| Path | 64 B | 4 KiB | 64 KiB | 128 KiB | Best in sweep |
|---|---:|---:|---:|---:|---:|
| RDMA-on-Asio / event callback | 0.609 Gbit/s | 7.357 Gbit/s | 8.469 Gbit/s | 8.600 Gbit/s | 8.600 Gbit/s |
| RDMA-on-Asio / poll `use_future` | 0.184 Gbit/s | 5.938 Gbit/s | 8.440 Gbit/s | 8.650 Gbit/s | 8.650 Gbit/s |
| Native ND / poll | 0.904 Gbit/s | 8.168 Gbit/s | 8.861 Gbit/s | 8.811 Gbit/s | 8.929 Gbit/s |

RDMA read plateaus around `8-9 Gbit/s` for all three paths on this machine. That makes the
large-message read result look provider/platform/operation limited in the current single-host
NetworkDirect setup, while small-message read still needs cleaner callback-token profiling.

Next performance work:

- Add `rdma_read_write_bench --mode poll --token-type callback` so read/write poll mode can be
  compared against native ND poll without `use_future` overhead.
- Add `--validate full|sample|none` so benchmark validation cost is visible and configurable.
- Optimize the SGE fast path for single-buffer read/write/send/recv operations: stack native SGE,
  no `std::distance()`, no repeated buffer scans, and no avoidable sglist resize.
- Measure custom associated allocators and operation recycling for small-message hot paths while
  preserving standard Asio async completion semantics.
- Keep `async_read` / `async_write` completion semantics intact. If a lower-overhead expert path
  is still justified, design it separately as `post_read` / `post_write` with explicit CQ polling
  and user-managed lifetime.
- Continue event-mode tuning by measuring CQ drain batch sizes and re-arm frequency.
- Done (2026-06-16): single-host Linux/`ibv` comparison against `perftest` (`ib_send_bw`,
  `ib_write_bw`, `ib_read_bw`) -- see the Linux table above. Extend to a multi-host (two-box)
  comparison when matching hardware on both ends is available.

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

- **Performance follow-up** -- finish the fair read/write poll/callback benchmark, SGE fast path,
  allocator measurements, event scheduler tuning, and multi-host `perftest` comparison. This work
  is tracked in [`docs/rdma_stress_performance_plan.md`](docs/rdma_stress_performance_plan.md) and
  [`docs/rdma_read_write_performance_optimization_plan.md`](docs/rdma_read_write_performance_optimization_plan.md).

**Done:** a deterministic, no-hardware unit-test suite (Asio-style harness,
`RDMA_BUILD_UNIT_TESTS`, default `ctest`) plus opt-in hardware integration/regression
tests (`RDMA_ENABLE_HARDWARE_TESTS` + `RDMA_TEST_ADDR`) now cover the public API,
buffer/op/config/error logic, and service-state guards -- see
[`docs/unit_test_plan.md`](docs/unit_test_plan.md).

See [`CLAUDE.md`](CLAUDE.md) for the detailed architecture and design notes.
