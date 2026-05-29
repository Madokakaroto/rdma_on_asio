# Project Guidelines

## Overview

This project provides a cross-platform RDMA abstraction layer with two goals:

1. **Unified RDMA interface** — Abstract away the differences between Windows NetworkDirect and Linux rdma-core behind a single portable API.
2. **Asio integration** — Seamlessly integrate with Asio's async model and C++20 coroutines (`co_await`).

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

### Build

```
cmake -B build
cmake --build build
```

Tests are compile-only on dev machines (no RDMA hardware). Runtime testing happens on a separate RDMA-capable machine.

## Line Endings

All text files use LF line endings (enforced via `.gitattributes`). Write all files with LF, not CRLF.
Exceptions: `.sln` and `.vcxproj` files use CRLF (Visual Studio requirement).
