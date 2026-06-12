#pragma once

#include <cassert>
#include <fcntl.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "asio.hpp"
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
inline bool set_nonblocking(int fd, asio::error_code& ec) {
  int opt = ::fcntl(fd, F_GETFL);
  if (opt == -1) {
    ec = last_system_error();
    return false;
  }
  if (::fcntl(fd, F_SETFL, opt | O_NONBLOCK) == -1) {
    ec = last_system_error();
    return false;
  }
  ec.clear();
  return true;
}

// Create a CM event channel and make its fd non-blocking.
inline native_event_channel_t* create_event_channel(asio::error_code& ec) {
  unique_rdma_event_channel_ptr channel{ ::rdma_create_event_channel() };
  if (!channel) {
    ec = last_system_error();
    return nullptr;
  }
  if (!set_nonblocking(channel->fd, ec)) {
    return nullptr;
  }
  return channel.release();
}

inline int create_cm_id(native_event_channel_t* channel, native_cm_id_t** id,
                        void* context, rdma_port_space port_space,
                        asio::error_code& ec) {
  int const rc = ::rdma_create_id(channel, id, context, port_space);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int migrate_id(native_cm_id_t* cm_id, native_event_channel_t* channel,
                      asio::error_code& ec) {
  int const rc = ::rdma_migrate_id(cm_id, channel);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int bind_addr(native_cm_id_t* cm_id, sockaddr const* addr,
                     asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_bind_addr(cm_id, const_cast<sockaddr*>(addr));
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int listen(native_cm_id_t* cm_id, int backlog, asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_listen(cm_id, backlog);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int resolve_addr(native_cm_id_t* cm_id, sockaddr* src, sockaddr* dst,
                        int timeout_ms, asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_resolve_addr(cm_id, src, dst, timeout_ms);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int resolve_route(native_cm_id_t* cm_id, int timeout_ms,
                         asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_resolve_route(cm_id, timeout_ms);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int connect(native_cm_id_t* cm_id, rdma_conn_param* conn_param,
                   asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_connect(cm_id, conn_param);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int accept(native_cm_id_t* cm_id, rdma_conn_param* conn_param,
                  asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_accept(cm_id, conn_param);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int disconnect(native_cm_id_t* cm_id, asio::error_code& ec) {
  if (cm_id == nullptr) {
    ec = asio::error::bad_descriptor;
    return -1;
  }
  int const rc = ::rdma_disconnect(cm_id);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

// Pull one CM event off the channel. On EAGAIN (no event yet) ec stays clear
// and *out_event is null --the caller keeps the op armed on the fd.
inline int get_cm_event(native_event_channel_t* channel,
                        native_cm_event_t** out_event, asio::error_code& ec) {
  *out_event = nullptr;
  int const rc = ::rdma_get_cm_event(channel, out_event);
  if (rc != 0 && errno != EAGAIN) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

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
inline void drain_cm_events(native_event_channel_t* channel,
                            native_cm_id_t* graceful_cm_id = nullptr) {
  if (channel == nullptr) {
    return;
  }
  asio::error_code ec;
  for (;;) {
    native_cm_event_t* event = nullptr;
    if (get_cm_event(channel, &event, ec) != 0) {
      break;  // EAGAIN (drained) or a hard error -- stop either way
    }
    unique_rdma_cm_event_ptr acked{ event };  // deleter acks on scope exit
    if (graceful_cm_id != nullptr &&
        acked->event == RDMA_CM_EVENT_ESTABLISHED) {
      asio::error_code ignored;
      disconnect(graceful_cm_id, ignored);
    }
  }
}

}
