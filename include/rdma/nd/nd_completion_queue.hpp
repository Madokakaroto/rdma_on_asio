#pragma once

#include <vector>

#include "asio/detail/mutex.hpp"
#include "asio/detail/op_queue.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/nd_device.hpp"
#include "rdma/nd/detail/nd_impl_types.hpp"
#include "rdma/nd/detail/nd_ops_verbs.hpp"
#include "rdma/nd/detail/nd_op_base.hpp"
#include "rdma/nd/detail/nd_config_derive.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"

namespace asio::rdma {

// Standalone poll-mode completion queue. Also holds the device (so a poll-mode
// queue_pair can create itself without an io_context) and a ready-op queue:
// data-plane ops that complete without a CQE (empty buffers / sync post errors)
// are queued here and drained by poll()/poll_one().
class nd_completion_queue {
public:
  nd_completion_queue(nd_device_ptr const& device,
                      nd_config_t const& config = {})
      : device_(device) {
    assert(device && device->adapter_);
    asio::error_code ec;

    effective_config_ = detail::derive_effective_config(config, device->info_);
    wc_buf_.resize(effective_config_.cq_poll_batch_
                       ? effective_config_.cq_poll_batch_
                       : detail::default_cq_poll_batch);

    handle_.reset(
        detail::create_overlapped_file(device->adapter_.Get(), ec));
    if (ec) {
      asio::detail::throw_error(ec);
    }

    detail::native_cq_init_attr cq_init_attr{
        .overlapped_handle_ = handle_.get(),
        .processor_group_ = 0,
        .processor_affinity_ = 0,
    };
    cq_.Attach(detail::verbs_ops::create_cq(
        device->adapter_.Get(), effective_config_.cqe_, cq_init_attr, ec));
    if (ec) {
      handle_.reset();
      asio::detail::throw_error(ec);
    }
  }

  ~nd_completion_queue() = default;
  // Owns a native CQ + overlapped handle + a mutex; not movable/copyable.
  nd_completion_queue(nd_completion_queue const&) = delete;
  nd_completion_queue& operator=(nd_completion_queue const&) = delete;
  nd_completion_queue(nd_completion_queue&&) = delete;
  nd_completion_queue& operator=(nd_completion_queue&&) = delete;

  std::size_t poll() {
    asio::error_code ec;
    auto n = poll(ec);
    if (ec) {
      asio::detail::throw_error(ec);
    }
    return n;
  }

  std::size_t poll(asio::error_code& ec) {
    std::size_t total = drain_ready();
    ULONG retrieved = 0;
    do {
      retrieved = detail::verbs_ops::poll_cq(
          cq_.Get(), wc_buf_.data(), static_cast<size_type>(wc_buf_.size()));
      for (ULONG i = 0; i < retrieved; ++i) {
        dispatch_completion(this, wc_buf_[i]);
      }
      total += retrieved;
    } while (retrieved != 0);
    return total;
  }

  std::size_t poll_one() {
    asio::error_code ec;
    auto n = poll_one(ec);
    if (ec) {
      asio::detail::throw_error(ec);
    }
    return n;
  }

  std::size_t poll_one(asio::error_code& ec) {
    if (auto* op = pop_ready()) {
      op->complete(this);
      return 1;
    }
    detail::native_wc_t result{};
    auto const retrieved = detail::verbs_ops::poll_cq(cq_.Get(), result);
    if (retrieved > 0) {
      dispatch_completion(this, result);
      return 1;
    }
    return 0;
  }

  detail::native_cq_t* native_handle() const noexcept { return cq_.Get(); }
  nd_device_ptr const& device() const noexcept { return device_; }
  nd_config_t const& effective_config() const noexcept {
    return effective_config_;
  }

  // Enqueue an op that completes without a CQE (empty buffer / sync post error).
  void push_ready(detail::rdma_verbs_op_base* op) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    ready_.push(op);
  }

private:
  static void dispatch_completion(void* owner, detail::native_wc_t const& wc) {
    if (!wc.RequestContext) return;
    auto* op = reinterpret_cast<detail::rdma_verbs_op_base*>(wc.RequestContext);
    op->ec_ = static_cast<nd_errc>(wc.Status);
    if (!op->ec_) {
      if (wc.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeSend &&
          wc.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeWrite) {
        op->bytes_transferred_ = wc.BytesTransferred;
      }
    } else {
      op->bytes_transferred_ = 0;
    }
    // Non-null owner so the handler upcall fires (mirrors ibv_completion_queue).
    op->complete(owner);
  }

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

  nd_device_ptr device_;
  nd_config_t effective_config_{};
  detail::nd2_completion_queue_ptr cq_;
  detail::unique_handle_t handle_;
  std::vector<detail::native_wc_t> wc_buf_;
  mutable asio::detail::mutex mutex_;
  asio::detail::op_queue<detail::rdma_verbs_op_base> ready_;
};

}
