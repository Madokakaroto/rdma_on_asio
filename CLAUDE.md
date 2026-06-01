# Project Guidelines

## Overview

This project provides a cross-platform RDMA abstraction layer with two goals:

1. **Unified RDMA interface** — Abstract away the differences between Windows NetworkDirect and Linux rdma-core behind a single portable API.
2. **Asio integration** — Seamlessly integrate with Asio's async model and C++20 coroutines (`co_await`).

> **`include/deprecated/` (temporary):** Holds an older version of the implementation, kept locally as a refactor reference only. Do NOT reference it from any new implementation, and do NOT add it to the build or tests. It is gitignored and not part of the project.

## RDMA Concept Comparison: NetworkDirect vs Linux Verbs

| Concept | Linux Verbs (libibverbs) | Windows NetworkDirect |
|---------|--------------------------|----------------------|
| Provider discovery | `ibv_get_device_list()` | `INetworkDirect::QueryAddressList()` |
| Device / Adapter | `ibv_device` / `ibv_context` (`ibv_open_device`) | `INDAdapter` (`OpenAdapter`) |
| Protection Domain | `ibv_pd` (`ibv_alloc_pd`) | Implicit in Adapter, no separate object |
| Completion Queue | `ibv_cq` (`ibv_create_cq`) | `INDCompletionQueue` (`CreateCompletionQueue`) |
| Completion Channel | `ibv_comp_channel` (fd, epoll) | OVERLAPPED / IOCP |
| Queue Pair | `ibv_qp` (`ibv_create_qp`) | `INDQueuePair` (`CreateQueuePair`) |
| Memory Region | `ibv_mr` (`ibv_reg_mr`) | `INDMemoryRegion` (`CreateMemoryRegion` + `Register`) |
| Memory Window | `ibv_mw` (`ibv_alloc_mw`) | `INDMemoryWindow` (`CreateMemoryWindow`) |
| Send / Recv | `ibv_post_send` / `ibv_post_recv` | `INDQueuePair::Send` / `Receive` |
| RDMA Read / Write | `ibv_post_send` (IBV_WR_RDMA_READ/WRITE) | `INDQueuePair::Read` / `Write` |
| Remote Key | `rkey` (uint32_t via MR) | `INDMemoryWindow::GetRemoteToken` + base addr |
| Connection management | `rdma_cm` (`rdma_connect` / `rdma_accept`) | `INDConnector` (`Connect` / `Accept`) |
| Listen | `rdma_listen` | `INDListen` (`Listen` / `GetConnectionRequest`) |
| Address resolution | `rdma_resolve_addr` / `rdma_resolve_route` | Direct `SOCKADDR`, no explicit resolution step |
| Event Channel | `rdma_event_channel` (fd, poll) | OVERLAPPED callback / IOCP notification |
| Shared Receive Queue | `ibv_srq` (`ibv_create_srq`) | `INDSharedReceiveQueue` |
| Completion polling | `ibv_poll_cq` / `ibv_req_notify_cq` | `INDCompletionQueue::GetResults` / IOCP `Notify` |
| Work Request | `struct ibv_send_wr` / `ibv_recv_wr` (linked list) | `ND2_SGE` array passed as method params |
| Scatter-Gather | `struct ibv_sge` | `ND2_SGE` |
| Port / GID | `ibv_port_attr` / `ibv_gid` | Implicit via adapter address list |
| Async model | fd + poll/epoll | OVERLAPPED + IOCP |

**Key differences:**

- **Async model**: Verbs uses fd + epoll; NetworkDirect uses OVERLAPPED/IOCP (native fit for asio's Windows backend).
- **Protection Domain**: Verbs manages PD explicitly; NetworkDirect subsumes it into Adapter.
- **Connection management**: Verbs requires rdma_cm for address/route resolution; NetworkDirect uses sockaddr directly.
- **Work Request submission**: Verbs uses linked-list chains (`ibv_send_wr`); NetworkDirect uses method calls with SGE array parameters.
- **Completion notification**: Verbs comp_channel is an fd for epoll; NetworkDirect uses IOCP events (basis for `nd_op_notify_wr` OVERLAPPED ops in this project).

## Architecture

### IO Object Layering

```
Public portspace (include/rdma/)
└── tcp.hpp                  — asio::rdma::tcp portspace, nested aliases: connector, listener, queue_pair

Platform IO objects (include/nd/)
├── nd_queue_pair.hpp        — data plane: async_send / async_recv / async_read / async_write
├── nd_connector.hpp        — control plane: open / async_connect / async_accept / async_disconnect
├── nd_listener.hpp          — server: open / bind / listen / async_get_connection_request
└── nd_completion_queue.hpp  — standalone poll-mode CQ (no IOCP)

Services (include/nd/detail/)
├── nd_io_completion_service.hpp  — per-io_context singleton, owns shared CQ + IOCP handle
├── nd_verbs_service.hpp          — manages QP per-impl, drives send/recv/read/write ops
├── nd_connector_service.hpp      — manages INDConnector per-impl, drives connect/accept/disconnect ops
└── nd_listener_service.hpp       — manages INDListen per-impl, drives GetConnectionRequest ops

Ops layer (include/nd/detail/)
├── nd_ops_verbs.hpp    — thin wrappers: create_overlapped_file, create_cq, create_qp, post_send, etc.
├── nd_ops_cm.hpp       — thin wrappers: create_connector, connect, accept, disconnect, bind_addr, etc.
├── nd_op_base.hpp      — OVERLAPPED-based base (connector/listener ops via IOCP)
├── nd_op_send/recv/read/write.hpp  — verbs_op_base (non-OVERLAPPED, dispatched via CQ RequestContext)
├── nd_op_connect.hpp   — connect completion op
├── nd_op_accept.hpp    — accept completion op (unused in new design, kept for reference)
├── nd_op_notify_wr.hpp — bridges CQ completions to IOCP scheduler
└── nd_op_get_connection_request.hpp — listener's GetConnectionRequest op

Device / Init
├── nd_device.hpp / nd_device_impl.hpp  — nd_device_manager_t singleton, adapter discovery
├── nd_use_device.hpp     — use_device() free functions (config or selector overload)
├── nd_config_derive.hpp  — derive_effective_config() + is_config_compatible()
└── nd_types.hpp          — nd_config_t, nd_remote_addr_t, type aliases
```

### Two Completion Modes

1. **IOCP mode** (default): `use_device()` initializes `nd_io_completion_service` which creates a shared CQ registered to the IOCP scheduler. `nd_queue_pair` attaches to this CQ. CQ completions are bridged to IOCP via `nd_op_notify_wr` (OVERLAPPED-based Notify call).

2. **Poll mode**: User creates a standalone `nd_completion_queue`, passes it to `nd_queue_pair::open(io_ctx, cq)`. User calls `cq.poll()` manually. No IOCP involvement for data-plane ops.

### Key Patterns

- **Service registration**: `asio::use_service<ServiceType>(io_ctx)` — asio creates the service on first access.
- **IO object impl**: `asio::detail::io_object_impl<ServiceType>` constructed with `(0, 0, io_ctx)`.
- **Config semantics**: `nd_config_t` fields default to 0; 0 means "auto-derive from device capabilities". Non-zero values are user constraints validated against device caps.
- **Async initiation**: All async methods use `asio::async_initiate<Token, Signature>(initiation, token, ...)`.
- **Error handling**: `asio::error_code&` out-param overloads + throw overloads via `asio::detail::throw_error(ec)`.
- **nd_connector_handle_t**: Moveable struct (connector + overlapped_handle + adapter) passed from listener to connection on the server side.

### ibv Backend (Linux verbs) — v4/v6 Design Decision

The `ibv` backend does **NOT** distinguish IPv4 vs IPv6 at the device layer. Unlike
NetworkDirect (whose `QueryAddressList` splits adapters into v4/v6 lists), a single
verbs device serves both families simultaneously — the v4/v6 distinction lives in the
**GID table** (per port: IPv4-mapped vs native IPv6 GIDs), not on the device.

**Decision: family is carried by the port space and consumed at rdma_cm connect time,
not at device selection.** This mirrors how asio handles it: `asio::ip::tcp` carries a
single `int family_` (`AF_INET`/`AF_INET6`) that flows straight into `socket(af, ...)`.
For rdma_cm the analog is the `sockaddr_in`/`sockaddr_in6` passed to `rdma_bind_addr` /
`rdma_resolve_addr` — rdma_cm then resolves the correct device + port + GID automatically
(a `rdma_cm_id` is not family-bound at `rdma_create_id` time, so this is even more flexible
than asio's open-time family pinning).

Consequences:
- `ibv_device_manager_t` stays **flat** (one device list, no v4/v6 split). Its job is
  capability query + per-device PD pre-allocation, not address-family selection.
- `get_first_available_device(ps, config)` keeps the `PortSpace` param for API parity with
  nd but **ignores** family — selection by family does not belong at this layer.
- The connection layer reads family via `tcp::family()` (returns the inner
  `asio::ip::tcp::family()`) and uses it only to build the sockaddr for rdma_cm.
- If device-level address enumeration is ever genuinely needed (e.g. listener address
  validation), scan the GID table with `ibv_query_gid_table()` — each `ibv_gid_entry`
  carries `gid_type` (RoCE v1/v2) and `ndev_ifindex` (→ netdev → IP via `getifaddrs`).
  Defer this until a concrete need arises; do not maintain an eager address snapshot that
  can drift from the system routing table.

### ibv Backend — Address Resolution Hidden Inside async_connect

There are **two distinct kinds of "resolve"**, and only one is user-facing:

1. **DNS** — `tcp::resolver` (= `asio::ip::basic_resolver`): `"host:port"` → endpoint
   (`getaddrinfo`, no RDMA). User-facing and explicit, identical to nd: the user resolves a
   host string to an endpoint, then passes the endpoint to `async_connect`.
2. **RDMA address/route binding** — `rdma_resolve_addr` + `rdma_resolve_route`: take an
   already-resolved sockaddr and bind the `rdma_cm_id` to a local device + route/GID. This
   is NOT name resolution and has no asio equivalent.

**Decision: `rdma_resolve_addr` and `rdma_resolve_route` are hidden inside `async_connect`**
(server side: inside accept/connection setup), never exposed on the public API. They carry
no user decision and form a fixed, branchless chain, so the public connector API stays
identical to nd — `async_connect(endpoint, private_data, token)`. Internal chain, each
`wait` being an `async_wait` on the rdma_event_channel fd (`posix::stream_descriptor`) →
`rdma_get_cm_event()`:

```
rdma_create_id(event_channel, &id, RDMA_PS_TCP)
rdma_resolve_addr(id, nullptr, dstaddr, timeout)  ─wait─▶ RDMA_CM_EVENT_ADDR_RESOLVED
rdma_resolve_route(id, timeout)                   ─wait─▶ RDMA_CM_EVENT_ROUTE_RESOLVED
rdma_connect(id, &conn_param)                     ─wait─▶ RDMA_CM_EVENT_ESTABLISHED
```

When hiding them, mind these boundaries:
- **Timeout**: `rdma_resolve_addr`/`resolve_route` each take a timeout (nd has none). Keep a
  sane default in the impl; if it must be tunable, put it in `ibv_config_t`, NOT in the
  `async_connect` signature — keep the API cross-platform-uniform.
- **Error mapping**: `ADDR_ERROR` / `ROUTE_ERROR` / `UNREACHABLE` events must collapse into a
  single `error_code` on the `async_connect` callback (cf. the deprecated `rdma_core_errors`:
  `core_addr_error` / `core_route_error` / `core_unreachable`).
- **Not hidden**: the rdma_event_channel itself stays a connector/listener impl member (the
  fd the above waits run on); and DNS via `tcp::resolver` stays a separate, explicit user
  step (passing an endpoint vs a host string is the user's choice).

### ibv Backend — Multi-Stage CM Op via reactor_op + status::not_done

The three-step CM handshake (addr → route → connect) is driven by **a single
`reactor_op`** with an internal state machine, NOT three separate async waits. This is the
idiomatic asio way to model "one fd, many readiness events, one op still in flight": the op
stays registered on the rdma_event_channel fd and re-arms itself by returning
`status::not_done` until the terminal event, then returns `status::done` and the handler
fires exactly once. (Reference, with caveats below: the deprecated
`rdma_cm_connect_op` / `rdma_core_connection_service::start_connect_op`.)

Mechanism:
1. **Start**: `start_connect_op` calls `rdma_resolve_addr(...)` **synchronously** (it only
   *initiates*, does not block), then `reactor_.start_op(reactor::read_op, cm_channel->fd,
   reactor_data, op, ...)` to register the op on the event-channel fd. The first resolve is
   issued *before* the op enters epoll.
2. **On fd readiness** the reactor calls the op's `do_perform`: it does
   `rdma_get_cm_event()`. The fd is `O_NONBLOCK` (`fcntl(F_SETFL, O_NONBLOCK)`), so no event
   → `errno==EAGAIN` → return `status::not_done` (stay in epoll). A hard error → `done`. An
   event → feed the state machine.
3. **State machine** (`stage_`: `addr_resolve → addr_route → connect`): each intermediate
   stage, on its expected event (`ADDR_RESOLVED` / `ROUTE_RESOLVED`), **issues the next
   rdma_cm call and returns `status::not_done`** so the same op waits for the next event on
   the same fd. The terminal `connect` stage returns `status::done` on `ESTABLISHED`; any
   `*_ERROR` / `UNREACHABLE` / `REJECTED` sets `ec_` and returns `done`.
4. **Completion**: `done` triggers `do_complete`, which makes a single
   `binder1<Handler, error_code>` upcall. Both resolves are fully internal; the user sees
   one completion.

Two channels (control + data) each register their own fd with the reactor
(`register_descriptor` for `cm_channel->fd` and `cq_channel->fd`, each with its own
`per_descriptor_data`) — the Linux realization of the dual-event-channel design above.

**Pitfalls in the deprecated reference (do NOT copy verbatim):**
- **Self-assignment in the ctor**: `cm_id_(cm_id_)` instead of `cm_id_(cm_id)` — leaves
  `cm_id_` uninitialized (UB). Member-init order also runs after the base ctor already used
  the real `cm_id`, so the bug is masked there but the member is still garbage.
- **Swapped error-code mapping**: `ADDR_ERROR` → `core_route_error`, `ROUTE_ERROR` →
  `core_addr_error`, `CONNECT_ERROR` → `core_addr_error`. addr/route are crossed; fix the
  mapping when reimplementing.
- **CM event ack timing**: `unique_rdma_cm_event_ptr`'s deleter is `rdma_ack_cm_event`, so
  events are acked at scope exit. The code issues `rdma_resolve_route` / `rdma_connect`
  while the triggering event is still un-acked. Prefer copying out the needed fields and
  acking the event early, before initiating the next stage.

### Build

```
cmake -B build
cmake --build build
```

Tests are compile-only on dev machines (no RDMA hardware). Runtime testing happens on a separate RDMA-capable machine.

## Line Endings

All text files use LF line endings (enforced via `.gitattributes`). Write all files with LF, not CRLF.
Exceptions: `.sln` and `.vcxproj` files use CRLF (Visual Studio requirement).
