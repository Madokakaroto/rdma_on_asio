# rdma-on-asio

A cross-platform **RDMA abstraction layer** that integrates with
[Asio](https://think-async.com/Asio/)'s asynchronous model and C++20 coroutines (`co_await`).

Write your RDMA application **once** against a single portable API; the same source compiles
and runs on:

- **Linux** — libibverbs + rdma_cm (`ibv` backend, epoll-based)
- **Windows** — NetworkDirect (`nd` backend, IOCP-based)

## Highlights

- **One portable API.** `#include "rdma/rdma.hpp"` and use the backend-agnostic `rdma_*`
  types + the `tcp` port space. A compile-time `#if` selects the backend.
- **Native Asio integration.** Every operation is an `async_*` initiation that works with
  `co_await`, callbacks, futures, `asio::as_tuple`, etc.
- **Connection management like TCP.** `connector` / `listener` mirror Asio's
  acceptor/socket model; on Linux the `rdma_resolve_addr`/`resolve_route` handshake is hidden
  inside `async_connect`.
- **Two completion modes.** Event-driven (shared CQ bridged into the Asio reactor/IOCP) or
  manual poll-mode (`completion_queue::poll()`, no io_context required).

## Quick start

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
  rdma_queue_pair     qp(io);
  rdma_connector<tcp> conn(io);
  conn.open(qp);                                            // bind qp to this connector

  co_await conn.async_connect(tcp::endpoint(asio::ip::make_address(host), port), {}, nothrow);

  std::array<char, 4096> buf{};
  rdma_memory_region mr(dev, buf.data(), buf.size());       // register memory
  std::memcpy(buf.data(), "hello", 5);

  co_await qp.async_send(mr.cslice(0, 5), nothrow);         // RDMA SEND
  auto [ec, n] = co_await qp.async_recv(mr.slice(0, buf.size()), nothrow);  // RECV
  co_await conn.async_disconnect(nothrow);
}

int main() {
  asio::io_context io;
  auto& svc = use_device(io);            // discover device, set up shared CQ
  asio::co_spawn(io, client(io, svc.get_device(), "10.0.0.1", 5000), asio::detached);
  io.run();
}
```

A complete, runnable echo client/server is in
[`tests/rdma/test_rdma_echo.cpp`](tests/rdma/test_rdma_echo.cpp).

## Public API

Include `rdma/rdma.hpp`. All names live in `namespace asio::rdma`.

| Type / call | Purpose |
|-------------|---------|
| `use_device(io, config = {})` | Discover a device and initialize the per-`io_context` completion service |
| `rdma_device_ptr` | Handle to the selected device (from `use_device(...).get_device()`) |
| `rdma_memory_region` | RAII memory region; `slice()` / `cslice()` produce send/recv buffers |
| `rdma_connector<tcp>` | Control plane: `open(qp)` / `async_connect` / `async_accept` / `async_disconnect` |
| `rdma_listener<tcp>` | Server: `open` / `bind(endpoint)` / `listen` / `async_get_connection_request` |
| `rdma_queue_pair` | Data plane: `async_send` / `async_recv` / `async_read` / `async_write` |
| `rdma_completion_queue` | Standalone poll-mode CQ |
| `tcp` | Port space: `tcp::endpoint`, `tcp::resolver`, and `tcp::{queue_pair,connector,listener}` |
| `rdma_config_t` | Capacities (CQ depth, WR/SGE limits, …); `0` = auto-derive from device caps |

`rdma_connector`/`rdma_listener` are templated on the port space (they carry the endpoint
type); `rdma_queue_pair` and friends are not (the data plane is port-space-agnostic).

## Requirements

- A C++20 compiler (coroutines, concepts).
- CMake ≥ 3.20.
- Submodules: `third_party/asio`, `third_party/networkdirect` — `git submodule update --init`.
- **Linux:** rdma-core dev packages —
  `sudo apt install libibverbs-dev librdmacm-dev` (or `dnf install rdma-core-devel`).
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
  ibv/          Linux libibverbs + rdma_cm backend
  nd/           Windows NetworkDirect backend
tests/
  rdma/         cross-platform tests (rdma_* API)   — built for either backend
  ibv/  nd/     backend-specific tests
```

## Status

- **ibv (Linux):** implemented and verified end-to-end over RoCE (device discovery,
  connect/accept/listen, send/recv/read/write, MR, dual completion modes, echo).
- **nd (Windows):** implemented against the same public surface; build to be verified on Windows.

## TODO

- **Cancellation** — wire up per-operation cancellation (Asio cancellation slots) across the
  CM and verbs ops; currently `cancel()` only cancels pending reactor ops.
- **Unit tests** — broaden coverage badly needed: config derivation, error mapping,
  MR slicing/bounds, completion dispatch, poll-mode CQ, connector/listener edge cases.
- **Benchmark** — add a throughput/latency benchmark harness (send/recv, RDMA read/write).
- **Compare against `perftest`** — publish numbers next to `ib_send_bw` / `ib_read_bw` /
  `ib_write_lat` (rdma-core `perftest`) to quantify the abstraction's overhead.

See [`CLAUDE.md`](CLAUDE.md) for the detailed architecture and design notes.
