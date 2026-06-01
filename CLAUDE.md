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

Two ways to spell the same types (both portable):

```cpp
#include "rdma/rdma.hpp"
using namespace asio::rdma;

// (a) backend-agnostic aliases (rdma/rdma_types.hpp)
rdma_queue_pair        qp(io);
rdma_connector<tcp>    conn(io);     // connector/listener stay templated on the port space
rdma_listener<tcp>     lis(io);
rdma_completion_queue  cq(dev);
rdma_memory_region     mr(dev, p, n);
rdma_device_ptr        dev = use_device(io).get_device();

// (b) the tcp port space (include/rdma/tcp.hpp) — equivalent
tcp::queue_pair  qp(io);
tcp::connector   conn(io);
tcp::endpoint    ep(addr, port);
```

`tcp.hpp` sets the backend macro and aliases `tcp::{queue_pair, connector, listener,
endpoint, resolver}`; `rdma_types.hpp` defines the `rdma_*` names on top; `rdma.hpp`
includes everything (incl. `use_device`, memory region, completion queue).

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
`rdma_types.hpp` → `tcp.hpp` (backend macro + connector/listener/queue_pair) + the active
backend's `{completion_queue, mr, device, use_device}` headers. Backend headers include
`rdma_commons.hpp` (leaf), never `tcp.hpp`/`rdma_types.hpp`.

**Data plane is portspace-agnostic.** `{nd,ibv}_queue_pair` and `{nd,ibv}_verbs_service`
are **non-template** (they only touch QP/CQ/MR). Only the control plane
(`{nd,ibv}_connector`/`listener`) is templated on `PortSpace` — it needs `PortSpace::endpoint`
and `PortSpace::rdma_type()`. Hence `rdma_connector`/`rdma_listener` are *template* aliases
while `rdma_queue_pair`/`rdma_completion_queue`/`rdma_memory_region`/`rdma_device` are plain.

### nd Backend (Windows, include/nd/)

```
IO objects:
  nd_queue_pair.hpp       — async_send / async_recv / async_read / async_write
  nd_connector.hpp        — open / async_connect / async_accept / async_disconnect
  nd_listener.hpp         — open / bind(endpoint) / listen / async_get_connection_request
  nd_completion_queue.hpp — standalone poll-mode CQ
  nd_mr.hpp               — RAII memory region `nd_memory_region` + const_buffer / mutable_buffer
  nd_use_device.hpp       — use_device(io_ctx, config) / use_device(io_ctx, selector)

Services (detail/):
  nd_io_completion_service.hpp — per-io_context singleton: shared CQ + IOCP handle
  nd_verbs_service.hpp         — QP lifecycle + data-plane op dispatch
  nd_connector_service.hpp     — INDConnector lifecycle + connect/accept/disconnect
  nd_listener_service.hpp      — INDListen lifecycle + GetConnectionRequest

Types (detail/):
  nd_impl_types.hpp     — native aliases, RAII handles, nd_connector_handle_t, nd_sglist_t
  nd_config_derive.hpp  — derive_effective_config() + is_config_compatible()
  nd_device_impl.hpp    — provider/adapter discovery
```

### ibv Backend (Linux, include/ibv/)

```
IO objects:
  ibv_queue_pair.hpp       — async_send / async_recv / async_read / async_write
  ibv_connector.hpp        — open / async_connect / async_accept / async_disconnect
  ibv_listener.hpp         — open / bind(endpoint) / listen / async_get_connection_request
  ibv_completion_queue.hpp — standalone poll-mode CQ
  ibv_mr.hpp               — RAII memory region `ibv_memory_region` + const_buffer / mutable_buffer
  ibv_use_device.hpp       — use_device(io_ctx, config)

Services (detail/):
  ibv_io_completion_service.hpp — per-io_context singleton: shared CQ + comp_channel on epoll
  ibv_verbs_service.hpp         — QP lifecycle + data-plane op dispatch
  ibv_connector_service.hpp     — rdma_cm_id lifecycle + connect/accept/disconnect
  ibv_listener_service.hpp      — listening cm_id + GetConnectionRequest

Types (detail/):
  ibv_impl_types.hpp     — native aliases, RAII deleters, ibv_connector_handle_t, ibv_sglist_t
  ibv_config_derive.hpp  — derive_effective_config() + is_config_compatible()
  ibv_device_impl.hpp    — rdma_get_devices() discovery
```

### Unified Public API Surface

| Operation | Signature (both backends) |
|-----------|--------------------------|
| Init device | `use_device(io_ctx, config)` → `io_completion_service&` |
| Queue pair | `queue_pair(io_ctx, config)` or deferred `open(io_ctx, config)` |
| Connector client | `connector.open(qp, config)` |
| Connector server | `connector.open(native_handle&&, qp, config)` |
| Connect | `async_connect(endpoint, private_data, token)` → `void(error_code)` |
| Accept | `async_accept(private_data, token)` → `void(error_code)` |
| Disconnect | `async_disconnect(token)` → `void(error_code)` |
| Listener bind | `listener.bind(endpoint)` |
| Get conn req | `async_get_connection_request(token)` → `void(ec, native_handle, span<byte>)` |
| Send/Recv | `async_send(buffers, token)` / `async_recv(buffers, token)` → `void(ec, size_t)` |
| RDMA R/W | `async_read(buffers, remote_addr, token)` / `async_write(...)` |

### Two Completion Modes (both backends)

1. **Event-driven mode** (default): `use_device()` initializes the io_completion_service
   (IOCP on Windows, comp_channel+epoll on Linux). CQ completions are bridged into asio's
   scheduler. User drives `io_ctx.run()`.

2. **Poll mode**: User creates a standalone completion_queue, passes it to
   `queue_pair::open(io_ctx, cq)`. User calls `cq.poll()` / `cq.poll_one()` manually.

### Key Patterns

- **Service registration**: `asio::use_service<ServiceType>(io_ctx)` — asio creates the service on first access.
- **IO object impl**: `asio::detail::io_object_impl<ServiceType>` constructed with `(0, 0, io_ctx)`.
- **Config semantics**: `rdma_config_t` fields default to 0; 0 means "auto-derive from device capabilities". Non-zero values are user constraints validated against device caps.
- **Async initiation**: All async methods use `asio::async_initiate<Token, Signature>(initiation, token, ...)`.
- **Error handling**: `asio::error_code&` out-param overloads + throw overloads via `asio::detail::throw_error(ec)`.
- **Connector handle**: Move-only struct (`nd_connector_handle_t` / `ibv_connector_handle_t`) passed from listener to server-side connector.

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

## Line Endings

All text files use LF line endings (enforced via `.gitattributes`). Write all files with LF, not CRLF.
Exceptions: `.sln` and `.vcxproj` files use CRLF (Visual Studio requirement).
