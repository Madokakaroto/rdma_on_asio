#pragma once

#include <array>
#include <cassert>
#include <cerrno>
#include <vector>

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
  explicit ibv_completion_queue(ibv_device_ptr const& device,
                                ibv_config_t const& config = {})
      : device_(device) {
    assert(device && device->context_);
    asio::error_code ec;
    effective_config_ = detail::derive_effective_config(config, device->attr_);
    wc_buf_.resize(effective_config_.cq_poll_batch_
                       ? effective_config_.cq_poll_batch_
                       : 16);
    // Poll mode: no comp_channel (no event-driven notification).
    cq_.reset(detail::verbs_ops::create_cq(
        device->context_, static_cast<int>(effective_config_.cqe_), nullptr,
        nullptr, 0, ec));
    asio::detail::throw_error(ec);
  }

  ~ibv_completion_queue() = default;
  // Owns a native CQ + a mutex; not movable/copyable. Hold it by reference.
  ibv_completion_queue(ibv_completion_queue const&) = delete;
  ibv_completion_queue& operator=(ibv_completion_queue const&) = delete;
  ibv_completion_queue(ibv_completion_queue&&) = delete;
  ibv_completion_queue& operator=(ibv_completion_queue&&) = delete;

  std::size_t poll() {
    asio::error_code ec;
    return poll(ec);
  }

  // error_code& overload (parity with nd_completion_queue). ibv_poll_cq returns a
  // negative value on failure; that is surfaced through ec. On success ec is cleared.
  std::size_t poll(asio::error_code& ec) {
    ec.clear();
    std::size_t total = drain_ready();
    int n = 0;
    do {
      n = detail::verbs_ops::poll_cq(cq_.get(),
                                     static_cast<int>(wc_buf_.size()),
                                     wc_buf_.data());
      if (n < 0) {
        ec = make_system_error_code(EIO);
        break;
      }
      for (int i = 0; i < n; ++i) {
        dispatch(wc_buf_[i]);
      }
      total += static_cast<std::size_t>(n);
    } while (n > 0);
    return total;
  }

  std::size_t poll_one() {
    asio::error_code ec;
    return poll_one(ec);
  }

  std::size_t poll_one(asio::error_code& ec) {
    ec.clear();
    if (auto* op = pop_ready()) {
      op->complete(this);
      return 1;
    }
    detail::native_wc_t wc{};
    int const n = detail::verbs_ops::poll_cq(cq_.get(), 1, &wc);
    if (n < 0) {
      ec = make_system_error_code(EIO);
      return 0;
    }
    if (n > 0) {
      dispatch(wc);
      return 1;
    }
    return 0;
  }

  detail::native_cq_t* native_handle() const noexcept { return cq_.get(); }
  ibv_device_ptr const& device() const noexcept { return device_; }
  ibv_config_t const& effective_config() const noexcept {
    return effective_config_;
  }

  // Enqueue an op that completes without a CQE (empty buffer / sync post error).
  // Drained by poll()/poll_one(). Thread-safe w.r.t. the polling thread.
  void push_ready(detail::rdma_verbs_op_base* op) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    ready_.push(op);
  }

private:
  void dispatch(detail::native_wc_t const& wc) {
    if (auto* op = detail::resolve_verbs_op(wc)) {
      // Non-null owner so the handler upcall fires (nd passed nullptr here).
      op->complete(this);
    }
  }

  // Move the whole ready queue out under the lock, then complete each op without
  // holding it (a handler may push new ready ops; they drain on the next poll).
  std::size_t drain_ready() {
    asio::detail::op_queue<detail::rdma_verbs_op_base> local;
    {
      asio::detail::mutex::scoped_lock lock(mutex_);
      local.push(ready_);
    }
    std::size_t n = 0;
    while (auto* op = local.front()) {
      local.pop();
      ++n;
      op->complete(this);
    }
    return n;
  }

  detail::rdma_verbs_op_base* pop_ready() {
    asio::detail::mutex::scoped_lock lock(mutex_);
    auto* op = ready_.front();
    if (op) {
      ready_.pop();
    }
    return op;
  }

  ibv_device_ptr device_;
  ibv_config_t effective_config_;
  std::vector<detail::native_wc_t> wc_buf_;
  detail::unique_ibv_cq_ptr cq_;
  mutable asio::detail::mutex mutex_;
  asio::detail::op_queue<detail::rdma_verbs_op_base> ready_;
};

}
