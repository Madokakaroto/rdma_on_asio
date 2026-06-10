# Project Guidelines

## Overview

This project provides a cross-platform RDMA abstraction layer with two goals:

1. **Unified RDMA interface** — Abstract away the differences between Windows NetworkDirect and Linux rdma-core behind a single portable API.
2. **Asio integration** — Seamlessly integrate with Asio's async model and C++20 coroutines (`co_await`).

## Architecture

### Cross-Platform Design

The public API lives in `include/rdma/`. Users **include `rdma/rdma.hpp`** (the single
umbrella) and write fully backend-agnostic code against the `rdma_*` aliases + the `tcp`
port space. Compile-time `#if` selects the backend:

- **Windows** → `nd_*` (NetworkDirect, IOCP-based)
- **Linux** → `ibv_*` (libibverbs + rdma_cm, epoll-based)

The control plane (`connector`/`listener`) is port-space-bound and has two equivalent,
portable spellings; the data plane (`queue_pair`/`completion_queue`/`memory_region`) is
port-space-agnostic and is only spelled `rdma_*`:

```cpp
#include "rdma/rdma.hpp"
using namespace asio::rdma;

// data plane — backend-agnostic aliases only (not in the tcp port space)
rdma_queue_pair        qp(io);
rdma_completion_queue  cq(dev);
rdma_memory_region     mr(dev, p, n);
rdma_device_ptr        dev = rdma_device_manager_t::instance()
                                 .get_first_available_device(tcp::v4(), {});
use_device(io, dev);   // installs this device's shared CQ on io (config set here)

// control plane — two equivalent spellings:
// (a) backend-agnostic aliases (rdma/rdma_types.hpp)
rdma_connector<tcp>    conn(io);     // connector/listener stay templated on the port space
rdma_listener<tcp>     lis(io);
// (b) the tcp port space (include/rdma/tcp.hpp)
tcp::connector   conn2(io);
tcp::listener    lis2(io);
tcp::endpoint    ep(addr, port);
```

`tcp.hpp` sets the backend macro and aliases `tcp::{connector, listener, endpoint, resolver}`
(queue_pair is *not* exported here — it's decoupled from the port space); `rdma_types.hpp`
defines the `rdma_*` names (incl. `rdma_queue_pair`) on top; `rdma.hpp` includes everything
(incl. `use_device`, memory region, completion queue).

### Shared Layer (include/rdma/)

```
rdma.hpp                — umbrella entry: pulls the active backend + all of the below
rdma_commons.hpp        — rdma_config_t, rdma_remote_addr_t, mr_acccess_flag_t, buffer tags
                          (backend-independent VALUES; both backends' impl types include it)
rdma_types.hpp          — backend-agnostic ALIASES: rdma_connector<PS>/rdma_listener<PS>
                          (template aliases), rdma_queue_pair, rdma_completion_queue,
                          rdma_memory_region, rdma_device / rdma_device_ptr
rdma_buffer.hpp         — mr_buffer / mr_adapted_buffer_sequence concepts, buffer_size()
tcp.hpp                 — tcp port space + backend selection (ASIO_RDMA_BACKEND_{ND,VERBS})
detail/rdma_verbs_op.hpp — rdma_verbs_op_base / rdma_two_sided_op / rdma_one_sided_op
detail/rdma_op_{send,recv,read,write}.hpp — typed completion ops (shared by both backends)
```

Header graph (no cycles): `rdma.hpp` → `rdma_commons.hpp` + `rdma_types.hpp`;
`rdma_types.hpp` → `tcp.hpp` (backend macro + connector/listener) + the active backend's
`{queue_pair, completion_queue, mr, device, use_device}` headers. Backend headers include
`rdma_commons.hpp` (leaf), never `tcp.hpp`/`rdma_types.hpp`.

**Data plane is portspace-agnostic.** `{nd,ibv}_queue_pair` and `{nd,ibv}_verbs_service`
are **non-template** (they only touch QP/CQ/MR). Only the control plane
(`{nd,ibv}_connector`/`listener`) is templated on `PortSpace` — it needs `PortSpace::endpoint`
and `PortSpace::rdma_type()`. Hence `rdma_connector`/`rdma_listener` are *template* aliases
while `rdma_queue_pair`/`rdma_completion_queue`/`rdma_memory_region`/`rdma_device` are plain.

### nd Backend (Windows, include/nd/)

```
IO objects:
  nd_queue_pair.hpp       — async_send / async_recv / async_read / async_write; (cq) ctor = io_context-free poll mode
  nd_connector.hpp        — open(port_space) / assign / async_connect(qp,..) / async_accept(qp,..) / async_disconnect / get_remote_data
  nd_listener.hpp         — open(port_space) / bind(endpoint) / listen / async_get_connection
  nd_completion_queue.hpp — standalone poll-mode CQ
  nd_mr.hpp               — RAII memory region `nd_memory_region` + const_buffer / mutable_buffer
  nd_use_device.hpp       — use_device(io_ctx, device_ptr, config={}) -> void

Services (detail/):
  nd_service_device.hpp        — per-io_context: registered device_ptr + effective_config (use_device)
  nd_service_io_completion.hpp — per-io_context: shared CQ + IOCP notify (arm_notify)
  nd_service_verbs.hpp         — QP lifecycle + data-plane op dispatch
  nd_service_connector.hpp     — INDConnector lifecycle + connect/accept/disconnect
  nd_service_listener.hpp      — INDListen lifecycle + GetConnectionRequest

Types (detail/):
  nd_impl_types.hpp     — native aliases, RAII handles, nd_connector_handle_t, nd_sglist_t
  nd_config_derive.hpp  — derive_effective_config() + is_config_compatible()
  nd_device_impl.hpp    — provider/adapter discovery
```

### ibv Backend (Linux, include/ibv/)

```
IO objects:
  ibv_queue_pair.hpp       — async_send / async_recv / async_read / async_write; (cq) ctor = io_context-free poll mode
  ibv_connector.hpp        — open(port_space) / assign / async_connect(qp,..) / async_accept(qp,..) / async_disconnect / get_remote_data
  ibv_listener.hpp         — open(port_space) / bind(endpoint) / listen / async_get_connection
  ibv_completion_queue.hpp — standalone poll-mode CQ
  ibv_mr.hpp               — RAII memory region `ibv_memory_region` + const_buffer / mutable_buffer
  ibv_use_device.hpp       — use_device(io_ctx, device_ptr, config={}) -> void

Services (detail/):
  ibv_service_device.hpp        — per-io_context: registered device_ptr + effective_config (use_device)
  ibv_service_io_completion.hpp — per-io_context: shared CQ + comp_channel on epoll (arm_notify)
  ibv_service_verbs.hpp         — QP lifecycle + data-plane op dispatch
  ibv_service_connector.hpp     — rdma_cm_id lifecycle + connect/accept/disconnect
  ibv_service_listener.hpp      — listening cm_id + GetConnectionRequest

Types (detail/):
  ibv_impl_types.hpp     — native aliases, RAII deleters, ibv_connector_handle_t, ibv_sglist_t
  ibv_config_derive.hpp  — derive_effective_config() + is_config_compatible()
  ibv_device_impl.hpp    — rdma_get_devices() discovery
```

### Unified Public API Surface

| Operation | Signature (both backends) |
|-----------|--------------------------|
| Discover device | `rdma_device_manager_t::instance().get_first_available_device(port_space, config)` → `rdma_device_ptr` (first device whose caps satisfy the non-zero `config` constraints; `for_each_device(fn)` to iterate) |
| Init device | `use_device(io_ctx, device, config = {})` → `void` — installs the per-`io_context` completion service for `device` and stores the effective (operating) config; reusable across `io_context`s |
| Queue pair (event) | `queue_pair(io_ctx)` or deferred `bind(io_ctx)` — bound to the io_context's managed CQ |
| Queue pair (poll) | `queue_pair(cq)` or deferred `bind(cq)` — io_context-free; bound to a user CQ (config read from the holder) |
| Queue pair state | `is_bound()` → bool (bound to a completion mechanism); `bound_type()` → `completion_mode {none, event, poll}` |
| Connector open | `connector.open(port_space)` — create the cm_id/connector (client side); requires `use_device` on this io_context |
| Connector adopt | `connector.assign(native_handle&&)` — adopt a handle from the listener (server side) |
| Connect | `async_connect(qp, endpoint, private_data, token)` → `void(error_code)` — creates the QP, then connects |
| Accept | `async_accept(qp, private_data, token)` → `void(error_code)` — creates the QP, then accepts |
| Disconnect | `async_disconnect(token)` → `void(error_code)` |
| Peer private data | `connector.get_remote_data()` → `const_buffer` (client req on server, server reply on client) |
| Listener setup | `listener.open(port_space)` / `listener.bind(endpoint)` / `listener.listen(backlog)` (requires `use_device`) |
| Get connection | `async_get_connection(token)` → `void(ec, connector)`; fill form `async_get_connection(conn, token)` → `void(ec)` |
| Send/Recv | `async_send(buffers, token)` / `async_recv(buffers, token)` → `void(ec, size_t)` |
| RDMA R/W | `async_read(buffers, remote_addr, token)` / `async_write(...)` |

`private_data` is an `asio::const_buffer` (pass `asio::buffer(...)`); an empty buffer sends none.
**QP creation timing differs by backend**: on **ibv** the native QP is created on the connector's
cm_id during `async_connect` / `async_accept` (the connector calls `qp.make_create_qp_fn()`); on
**nd** the QP is created (and owned) at `queue_pair` construction and the connector borrows it via
`native_handle()`. Either way the QP gets `{device, config}` from the `device_service` and, in
event mode, `cq` from the `io_completion_service`; poll mode reads everything from the
`completion_queue`.

**Two per-io_context services, single responsibility each:** `device_service` holds the
registered `device_ptr` + `effective_config` (and answers `is_registered()` — the canonical
"use_device called?" predicate); `io_completion_service` owns the shared CQ + comp_channel/IOCP
notify (`arm_notify`). `use_device` wires both: derive the effective config, `io.initialize` the
CQ, then `dev.register_device` (so a CQ-init failure leaves the io_context unregistered).
Service→service deps are cached as references in the holder's ctor (e.g. `verbs_service` holds
`io_completion_service&`; `connector`/`listener` hold `device_service&`), never looked up per-op.

**Config is centralized in `use_device`.** There is a single `rdma_config_t` in three roles:
selection constraint (to `get_first_available_device`), device capability (`device->attr_`/`info_`),
and effective/operating config (`derive_effective_config(config, caps)`, stored in
`device_service`). `connector`/`listener` no longer take a config — their connection params
(`responder_resources`/`initiator_depth` from `inbound`/`outbound_read_limit_`) are read from
`device_service::get_effective_config()`. Opening a `connector`/`listener`/`queue_pair` without
`use_device` on that io_context fails with `ext_device_not_registered`
(`device_service::is_registered()`).

### Two Completion Modes (both backends)

1. **Event-driven mode** (default): `use_device(io, device)` registers the device (`device_service`)
   and initializes the `io_completion_service` (IOCP on Windows, comp_channel+epoll on Linux). CQ
   completions are bridged into asio's scheduler. User drives `io_ctx.run()`.

   The shared-CQ poller is **lock-free & thread-safe** for concurrent multi-thread submit +
   multi-thread `run()`: it's a single self-perpetuating op (`ibv_poll_wc_op` / `nd_poll_wc_op`)
   started lazily at the **first event-mode `queue_pair::bind(io)`** (`ensure_poller_started()`,
   one-time) and re-arming itself thereafter, so it is the only thing that touches the CQ —
   submitter threads just `post`. **Contract:** once started, the poller is outstanding work for
   the io_context's lifetime, so `io.run()` no longer returns on idle — stop via `io.stop()` /
   destruction. io_contexts that never bind an event-mode QP (poll-mode-only / control-plane-only)
   never start the poller and keep "run() returns when idle".

2. **Poll mode** (io_context-free data plane): User creates a standalone `completion_queue` (no
   comp_channel) and binds the QP via `queue_pair(cq)` (or `bind(cq)`) — **no io_context**. User
   calls `cq.poll()` / `cq.poll_one()`. The QP supplies `asio::system_executor` as the op's
   executor, so with a **non-io_context-bound token (callback / `use_future`)** the completion
   handler fires **inline on the polling thread** — the data path never touches an io_context.
   (With `use_awaitable`/an io_context-bound token, the handler's own executor wins and the
   completion posts back to that io_context instead.) The control-plane handshake
   (`connector`/`listener`) still runs on an io_context; only the data plane is io_context-free.
   Empty-buffer / sync-post-error completions are queued on the `completion_queue` and drained by
   `poll()` (so handlers only ever fire from within `poll()`).

### Key Patterns

- **Service registration**: `asio::use_service<ServiceType>(io_ctx)` — asio creates the service on first access.
- **IO object impl**: `connector`/`listener` use `asio::detail::io_object_impl<ServiceType>`
  constructed with `(0, 0, io_ctx)`. **`queue_pair` does NOT** — it owns the verbs-service
  `implementation_type` directly (so poll mode needs no io_context) and dispatches to the static
  (poll) or member (event) service entry points based on whether it holds an `io_context*`.
- **Config semantics**: `rdma_config_t` fields default to 0; 0 means "auto-derive from device capabilities". Non-zero values are user constraints validated against device caps.
- **Async initiation**: All async methods use `asio::async_initiate<Token, Signature>(initiation, token, ...)`.
- **Error handling**: `asio::error_code&` out-param overloads + throw overloads via `asio::detail::throw_error(ec)`.
- **Connector handle**: Move-only struct (`nd_connector_handle_t` / `ibv_connector_handle_t`) produced by the listener and adopted by the server-side connector via `assign`. `listener.async_get_connection` wraps this: it constructs a connector, calls `assign` with the handle + the peer's private data, and yields the ready-to-`async_accept` connector to the handler.

### Platform-Specific Design Notes

**nd (Windows):**
- Async control-plane ops use `nd_op_base` (inherits `asio::detail::operation` with OVERLAPPED semantics).
- Data-plane ops use `rdma_verbs_op_base` dispatched via CQ `RequestContext` → `nd_op_notify_wr` bridges to IOCP.
- NetworkDirect splits adapters by v4/v6; `tcp::get_adapters(provider)` selects by family.

**ibv (Linux):**
- Async control-plane ops use `reactor_op` with internal state machine (returns `status::not_done` to re-arm on the same event-channel fd).
- Data-plane ops use `rdma_verbs_op_base` dispatched via `ibv_cq` `wr_id` → `ibv_cq_notify_op` bridges to epoll reactor.
- `rdma_resolve_addr` + `rdma_resolve_route` are hidden inside `async_connect` (multi-stage CM op). The user sees one completion.
- Device manager is flat (no v4/v6 split); address family is consumed at rdma_cm connect time via the sockaddr passed in the endpoint.
- QP creation is deferred: `ibv_queue_pair` provides a `make_create_qp_fn()` callback that the connector invokes once the cm_id has a resolved context.

### Build

```
cmake -B build
cmake --build build
```

Select the backend with `-DRDMA_BACKEND=ibv` (Linux, default) or `nd` (Windows).

### Tests

```
tests/nd/    — NetworkDirect-specific tests (use nd_* types)
tests/ibv/   — libibverbs-specific tests (use ibv_* types)
tests/rdma/  — cross-platform tests (use only rdma_* aliases + rdma/rdma.hpp); built for either backend
```

`tests/rdma/test_rdma_echo.cpp` is the canonical portable example: one source, backend chosen
by the build. Runtime testing needs RDMA hardware; on Linux the ibv + rdma echo tests have been
verified end-to-end over RoCE (`--server` / `--client <ip>` / `--port`).

## TODO

- **Multi-thread completion-notification stress test** — the event-mode shared-CQ poller is
  designed to be lock-free / thread-safe under multi-thread `io_context::run()` + concurrent
  `async_send` from multiple threads (single self-perpetuating poller; submitters touch no service
  state). Add a stress test that exercises this: several `run()` threads + several threads posting
  concurrently (over multiple QPs), asserting high-concurrency stability, a consistent completion
  count, and a clean `io.stop()` exit. (Hard to make a deterministic race assertion; aim for a
  long-running soak + counter check.)

## Code Comments

Do not use non-ASCII characters in source code comments. MSVC triggers C4819 warnings when it encounters characters outside the current code page (936/GBK on Chinese Windows). Use ASCII equivalents instead (e.g. `--` instead of `—`).

## Line Endings

All text files use LF line endings (enforced via `.gitattributes`). Write all files with LF, not CRLF.
Exceptions: `.sln` and `.vcxproj` files use CRLF (Visual Studio requirement).
