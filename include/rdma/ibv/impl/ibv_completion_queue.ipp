#ifndef RDMA_IBV_IMPL_IBV_COMPLETION_QUEUE_IPP
#define RDMA_IBV_IMPL_IBV_COMPLETION_QUEUE_IPP

#include <cassert>
#include <cerrno>

#include "rdma/ibv/ibv_completion_queue.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

ibv_completion_queue::ibv_completion_queue(ibv_device_ptr const& device,
                                           ibv_config_t const& config)
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

std::size_t ibv_completion_queue::poll() {
  asio::error_code ec;
  auto const n = poll(ec);
  if (ec) {
    asio::detail::throw_error(ec);
  }
  return n;
}

// error_code& overload (parity with nd_completion_queue). ibv_poll_cq returns a
// negative value on failure; that is surfaced through ec. On success ec is cleared.
std::size_t ibv_completion_queue::poll(asio::error_code& ec) {
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

std::size_t ibv_completion_queue::poll_one() {
  asio::error_code ec;
  auto const n = poll_one(ec);
  if (ec) {
    asio::detail::throw_error(ec);
  }
  return n;
}

std::size_t ibv_completion_queue::poll_one(asio::error_code& ec) {
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

// Enqueue an op that completes without a CQE (empty buffer / sync post error).
// Drained by poll()/poll_one(). Thread-safe w.r.t. the polling thread.
void ibv_completion_queue::push_ready(detail::rdma_verbs_op_base* op) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  ready_.push(op);
}

void ibv_completion_queue::dispatch(detail::native_wc_t const& wc) {
  if (auto* op = detail::resolve_verbs_op(wc)) {
    // Non-null owner so the handler upcall fires (nd passed nullptr here).
    op->complete(this);
  }
}

// Move the whole ready queue out under the lock, then complete each op without
// holding it (a handler may push new ready ops; they drain on the next poll).
std::size_t ibv_completion_queue::drain_ready() {
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

detail::rdma_verbs_op_base* ibv_completion_queue::pop_ready() {
  asio::detail::mutex::scoped_lock lock(mutex_);
  auto* op = ready_.front();
  if (op) {
    ready_.pop();
  }
  return op;
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_COMPLETION_QUEUE_IPP
