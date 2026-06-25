#pragma once

#include <array>
#include <cassert>
#include <cerrno>
#include <vector>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/mutex.hpp"
#include "asio/detail/op_queue.hpp"
#include "rdma/ibv/ibv_device.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_config_derive.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"
#include "rdma/ibv/detail/ibv_op_complete.hpp"
#include "rdma/ibv/detail/ibv_ops_verbs.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"

namespace asio::rdma {

// Standalone completion queue (mirrors nd_completion_queue). Poll mode: the user
// drives poll()/poll_one() and handlers fire synchronously; no io_context
// involvement. The QP is opened against this CQ via ibv_queue_pair(cq, ...).
//
// The CQ also holds the device (so the poll-mode QP can create itself without an
// io_context) and a small ready-op queue: data-plane ops that complete without a
// real CQE (empty buffers / synchronous post errors) are queued here and drained
// by poll()/poll_one(), preserving "handlers only fire during poll()".
class ibv_completion_queue {
public:
  ASIO_DECL explicit ibv_completion_queue(ibv_device_ptr const& device,
                                          ibv_config_t const& config = {});

  ~ibv_completion_queue() = default;
  // Owns a native CQ + a mutex; not movable/copyable. Hold it by reference.
  ibv_completion_queue(ibv_completion_queue const&) = delete;
  ibv_completion_queue& operator=(ibv_completion_queue const&) = delete;
  ibv_completion_queue(ibv_completion_queue&&) = delete;
  ibv_completion_queue& operator=(ibv_completion_queue&&) = delete;

  ASIO_DECL std::size_t poll();

  // error_code& overload (parity with nd_completion_queue). ibv_poll_cq returns a
  // negative value on failure; that is surfaced through ec. On success ec is cleared.
  ASIO_DECL std::size_t poll(asio::error_code& ec);

  ASIO_DECL std::size_t poll_one();

  ASIO_DECL std::size_t poll_one(asio::error_code& ec);

  detail::native_cq_t* native_handle() const noexcept { return cq_.get(); }
  ibv_device_ptr const& device() const noexcept { return device_; }
  ibv_config_t const& effective_config() const noexcept {
    return effective_config_;
  }

  // Enqueue an op that completes without a CQE (empty buffer / sync post error).
  // Drained by poll()/poll_one(). Thread-safe w.r.t. the polling thread.
  ASIO_DECL void push_ready(detail::rdma_verbs_op_base* op);

private:
  ASIO_DECL void dispatch(detail::native_wc_t const& wc);

  // Move the whole ready queue out under the lock, then complete each op without
  // holding it (a handler may push new ready ops; they drain on the next poll).
  ASIO_DECL std::size_t drain_ready();

  ASIO_DECL detail::rdma_verbs_op_base* pop_ready();

  ibv_device_ptr device_;
  ibv_config_t effective_config_;
  std::vector<detail::native_wc_t> wc_buf_;
  detail::unique_ibv_cq_ptr cq_;
  mutable asio::detail::mutex mutex_;
  asio::detail::op_queue<detail::rdma_verbs_op_base> ready_;
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/impl/ibv_completion_queue.ipp"
#endif
