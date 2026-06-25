#pragma once

#include <vector>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
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
  ASIO_DECL nd_completion_queue(nd_device_ptr const& device,
                                nd_config_t const& config = {});

  ~nd_completion_queue() = default;
  // Owns a native CQ + overlapped handle + a mutex; not movable/copyable.
  nd_completion_queue(nd_completion_queue const&) = delete;
  nd_completion_queue& operator=(nd_completion_queue const&) = delete;
  nd_completion_queue(nd_completion_queue&&) = delete;
  nd_completion_queue& operator=(nd_completion_queue&&) = delete;

  ASIO_DECL std::size_t poll();

  ASIO_DECL std::size_t poll(asio::error_code& ec);

  ASIO_DECL std::size_t poll_one();

  ASIO_DECL std::size_t poll_one(asio::error_code& ec);

  detail::native_cq_t* native_handle() const noexcept { return cq_.Get(); }
  nd_device_ptr const& device() const noexcept { return device_; }
  nd_config_t const& effective_config() const noexcept {
    return effective_config_;
  }

  // Enqueue an op that completes without a CQE (empty buffer / sync post error).
  ASIO_DECL void push_ready(detail::rdma_verbs_op_base* op);

private:
  ASIO_DECL static void dispatch_completion(void* owner,
                                            detail::native_wc_t const& wc);

  ASIO_DECL std::size_t drain_ready();

  ASIO_DECL detail::rdma_verbs_op_base* pop_ready();

  nd_device_ptr device_;
  nd_config_t effective_config_{};
  detail::nd2_completion_queue_ptr cq_;
  detail::unique_handle_t handle_;
  std::vector<detail::native_wc_t> wc_buf_;
  mutable asio::detail::mutex mutex_;
  asio::detail::op_queue<detail::rdma_verbs_op_base> ready_;
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/impl/nd_completion_queue.ipp"
#endif
