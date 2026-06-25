#pragma once

#include <cassert>
#include <fcntl.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "asio.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

// Thin wrappers over rdma_cm / libibverbs control-plane calls, mirroring the
// style of nd/detail/nd_ops_cm.hpp: each call takes an asio::error_code& out
// param. rdma_* functions return int (0 ok, non-zero with errno set) or a
// pointer (null on failure with errno set); we map errno via ibv_error.hpp.
namespace asio::rdma::detail {

// Make a file descriptor non-blocking so rdma_get_cm_event returns EAGAIN
// instead of blocking inside the reactor callback.
ASIO_DECL bool set_nonblocking(int fd, asio::error_code& ec);

// Create a CM event channel and make its fd non-blocking.
ASIO_DECL native_event_channel_t* create_event_channel(asio::error_code& ec);

ASIO_DECL int create_cm_id(native_event_channel_t* channel, native_cm_id_t** id,
                           void* context, rdma_port_space port_space,
                           asio::error_code& ec);

ASIO_DECL int migrate_id(native_cm_id_t* cm_id, native_event_channel_t* channel,
                         asio::error_code& ec);

ASIO_DECL int bind_addr(native_cm_id_t* cm_id, sockaddr const* addr,
                        asio::error_code& ec);

ASIO_DECL int listen(native_cm_id_t* cm_id, int backlog, asio::error_code& ec);

ASIO_DECL int resolve_addr(native_cm_id_t* cm_id, sockaddr* src, sockaddr* dst,
                           int timeout_ms, asio::error_code& ec);

ASIO_DECL int resolve_route(native_cm_id_t* cm_id, int timeout_ms,
                            asio::error_code& ec);

ASIO_DECL int connect(native_cm_id_t* cm_id, rdma_conn_param* conn_param,
                      asio::error_code& ec);

ASIO_DECL int accept(native_cm_id_t* cm_id, rdma_conn_param* conn_param,
                     asio::error_code& ec);

ASIO_DECL int disconnect(native_cm_id_t* cm_id, asio::error_code& ec);

// Pull one CM event off the channel. On EAGAIN (no event yet) ec stays clear
// and *out_event is null --the caller keeps the op armed on the fd.
ASIO_DECL int get_cm_event(native_event_channel_t* channel,
                           native_cm_event_t** out_event, asio::error_code& ec);

// Drain + ack every currently-queued CM event on a (non-blocking) channel.
// Must be called before rdma_destroy_id on teardown: rdma_destroy_id blocks
// until all reported events have been acknowledged. Each event is wrapped in a
// unique_rdma_cm_event_ptr whose deleter calls rdma_ack_cm_event.
// If graceful_cm_id is non-null and a drained event is ESTABLISHED, the
// connection came up but was never processed (disconnect()'s cancel_ops aborted
// the in-flight op before it drained the event) -- issue a graceful
// rdma_disconnect so the peer gets a DREQ instead of an abrupt destroy. Only
// fires in that narrow race window; on the normal path ESTABLISHED was already
// consumed by the connect/accept op. See docs/cancellation_stage1_object.md A.7.
ASIO_DECL void drain_cm_events(native_event_channel_t* channel,
                               native_cm_id_t* graceful_cm_id = nullptr);

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_ops_cm.ipp"
#endif
