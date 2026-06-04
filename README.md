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
  manual poll-mode (`completion_queue::poll()`) with an **io_context-free data plane** — bind the
  queue pair to a standalone CQ and complete data-plane ops on your own polling thread.

## Quick start

The data plane (`async_send` / `async_recv` / `async_read` / `async_write`) can deliver its
completions two ways. Both examples below share the same setup: discover a device via
`rdma_device_manager_t`, hand it to `use_device(io, dev)` (which installs the per-`io_context`
completion service and is where the operating config is set), then run `connector` / `listener`
on that `io_context`. One device can be shared across several `io_context`s — call
`use_device` with the same `rdma_device_ptr` on each.

### Event-driven mode (default)

`use_device()` installs a shared completion queue that is bridged into Asio's reactor (IOCP on
Windows, comp_channel + epoll on Linux). Verbs-op completions are posted to the `io_context`,
so they compose with `co_await`, callbacks, futures, `asio::as_tuple`, etc. — you just drive
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
                              asio::buffer(hello), nothrow); // QP is created during connect

  std::array<char, 4096> buf{};
  rdma_memory_region mr(dev, buf.data(), buf.size());       // register memory
  std::memcpy(buf.data(), "hello", 5);

  co_await qp.async_send(mr.cslice(0, 5), nothrow);         // RDMA SEND
  auto [ec, n] = co_await qp.async_recv(mr.slice(0, buf.size()), nothrow);  // RECV
  co_await conn.async_disconnect(nothrow);
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
listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
listener.listen();

auto [ec, conn] = co_await listener.async_get_connection(nothrow);  // -> a connector
// conn.get_remote_data() is the client's private data

rdma_queue_pair qp(io);
std::string reply = "server-hello";
co_await conn.async_accept(qp, asio::buffer(reply), nothrow);        // QP created during accept
// ... qp.async_recv / qp.async_send ...
```

### Poll mode (manual completion queue)

Create a standalone `rdma_completion_queue`, bind the queue pair to it with the
**`(cq)` constructor — no `io_context`** — and reap data-plane completions yourself with
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
  rdma_queue_pair       qp(cq);        // poll-mode QP — bound to cq, NO io_context

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
| `rdma_device_manager_t::instance()` | Process-wide device registry; `get_first_available_device(port_space, config)` → `rdma_device_ptr` (first device whose caps satisfy the non-zero `config` constraints) |
| `use_device(io, device, config = {})` | Install the per-`io_context` completion service for `device`; sets the operating config. Returns `void`; share one `device` across multiple `io_context`s by calling it on each |
| `rdma_device_ptr` | Handle to a device (from `get_first_available_device`) |
| `rdma_memory_region` | RAII memory region; `slice()` / `cslice()` produce send/recv buffers |
| `rdma_connector<tcp>` | Control plane: `open(port_space)` / `async_connect(qp, ep, pd)` / `async_accept(qp, pd)` / `async_disconnect` / `get_remote_data()` |
| `rdma_listener<tcp>` | Server: `open(port_space)` / `bind(endpoint)` / `listen` / `async_get_connection` → `(ec, connector)` |
| `rdma_queue_pair` | Data plane: `async_send` / `async_recv` / `async_read` / `async_write`. Binds to a completion mechanism: `rdma_queue_pair(io)` = event-driven; `rdma_queue_pair(cq)` = poll-mode (io_context-free data plane). `bind()` (deferred) / `is_bound()` / `bound_type()` → `completion_mode` |
| `rdma_completion_queue` | Standalone poll-mode CQ; `poll()` / `poll_one()` |
| `tcp` | Port space: `tcp::endpoint`, `tcp::resolver`, and `tcp::{connector,listener}` (the data-plane `queue_pair` is port-space-agnostic — use `rdma_queue_pair`) |
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
