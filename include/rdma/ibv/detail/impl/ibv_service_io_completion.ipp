#ifndef RDMA_IBV_IMPL_IBV_SERVICE_IO_COMPLETION_IPP
#define RDMA_IBV_IMPL_IBV_SERVICE_IO_COMPLETION_IPP

#include "rdma/ibv/detail/ibv_service_io_completion.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_io_completion_service::ibv_io_completion_service(asio::execution_context& ctx)
    : base_type(ctx)
    , reactor_(asio::use_service<asio::detail::reactor>(ctx))
    , scheduler_(asio::use_service<asio::detail::scheduler>(ctx)) {
  reactor_.init_task();  // install the epoll task in the scheduler
}

void ibv_io_completion_service::shutdown() {
  if (comp_channel_) {
    // Cancels the poller if armed (its do_complete runs with owner == null).
    reactor_.deregister_descriptor(comp_channel_->fd, cq_reactor_data_,
                                   false);
    reactor_.cleanup_descriptor_data(cq_reactor_data_);
  }
  cq_.reset();
  comp_channel_.reset();
}

// Create the shared CQ + comp_channel on the reactor. The device is used
// transiently for its context (not stored --that's ibv_device_service's job);
// cqe is the already-derived CQ depth. Idempotency self-guards on cq_.
void ibv_io_completion_service::initialize(ibv_device_ptr const& device,
                                           std::uint32_t cqe,
                                           std::uint32_t poll_batch,
                                           asio::error_code& ec) {
  if (cq_) {
    ec = make_error_code(rdma_errc::already_registered);
    return;
  }
  if (!device || !device->context_) {
    ec = make_error_code(rdma_errc::invalid_device);
    return;
  }

  comp_channel_.reset(verbs_ops::create_comp_channel(device->context_, ec));
  if (ec) {
    return;
  }
  cq_.reset(verbs_ops::create_cq(device->context_, static_cast<int>(cqe),
                                 nullptr, comp_channel_.get(), 0, ec));
  if (ec) {
    comp_channel_.reset();
    return;
  }
  if (int err = reactor_.register_descriptor(comp_channel_->fd,
                                             cq_reactor_data_)) {
    ec = make_system_error_code(err);
    cq_.reset();
    comp_channel_.reset();
    return;
  }
  poll_batch_ = poll_batch ? static_cast<int>(poll_batch) : 16;
  wc_buf_.resize(static_cast<std::size_t>(poll_batch_));
  ec.clear();
}

// Start the self-perpetuating CQ poller. Idempotent + thread-safe: the first
// caller (the first event-mode queue_pair to bind on this io_context) fires it;
// everyone else is a no-op. After this the poller re-arms itself forever, so no
// submitter ever touches the CQ/reactor again.
void ibv_io_completion_service::ensure_poller_started() {
  if (!poller_started_.exchange(true, std::memory_order_acq_rel)) {
    arm_poller();
  }
}

asio::detail::reactor_op::status
ibv_io_completion_service::ibv_poll_wc_op::do_perform(
    asio::detail::reactor_op* base) {
  auto* o = static_cast<ibv_poll_wc_op*>(base);
  ibv_io_completion_service* svc = o->svc_;

  // Consume a comp_channel event if queued; re-arm CQ notification for the
  // next one. fd is O_NONBLOCK so this is EAGAIN when none.
  native_cq_t* ev_cq = nullptr;
  void* ev_ctx = nullptr;
  if (verbs_ops::get_cq_event(svc->comp_channel_.get(), &ev_cq, &ev_ctx) ==
      0) {
    verbs_ops::ack_cq_events(ev_cq, 1);
    asio::error_code ec;
    verbs_ops::req_notify_cq(ev_cq, false, ec);
  }

  svc->poll_into(o->completed_);
  return o->completed_.empty() ? not_done : done;
}

void ibv_io_completion_service::ibv_poll_wc_op::do_complete(
    void* owner, asio::detail::operation* base,
    asio::error_code const& /*result_ec*/,
    std::size_t /*bytes_transferred*/) {
  auto* o = static_cast<ibv_poll_wc_op*>(base);
  o->svc_->on_poll_complete(owner, o->completed_);
}

// Drain at most a bounded number of CQ batches, resolving each work completion
// to its verbs op.
void ibv_io_completion_service::poll_into(
    asio::detail::op_queue<rdma_verbs_op_base>& out) {
  int n = 0;
  constexpr int max_batches_per_turn = 4;
  int batches = 0;
  do {
    n = verbs_ops::poll_cq(cq_.get(), static_cast<int>(wc_buf_.size()),
                           wc_buf_.data());
    for (int i = 0; i < n; ++i) {
      if (auto* op = resolve_verbs_op(wc_buf_[i])) {
        out.push(op);
      }
    }
    ++batches;
  } while (n == static_cast<int>(wc_buf_.size()) &&
           batches < max_batches_per_turn);
}

// Arm (or re-arm) the single poller. To close the post-before-notify race (a WR
// completing between ibv_post_* and ibv_req_notify_cq on fast loopback) we
// req_notify then poll immediately; if completions are already present we queue
// the poller's completion directly, else we wait on the comp_channel fd. Only
// ever called from ensure_poller_started() (one-time) or on_poll_complete()
// (after the poller's own dispatch), so it is never run concurrently.
void ibv_io_completion_service::arm_poller() {
  asio::error_code ec;
  verbs_ops::req_notify_cq(cq_.get(), false, ec);
  poll_into(poller_.completed_);
  if (!poller_.completed_.empty()) {
    scheduler_.post_immediate_completion(&poller_, false);
  }
  else {
    reactor_.start_op(asio::detail::reactor::read_op, comp_channel_->fd,
                      cq_reactor_data_, &poller_, false, false);
  }
}

// Poller completion: dispatch the drained ops, then re-arm unconditionally.
// owner == nullptr means reactor shutdown --free handlers without an upcall and
// do not re-arm.
void ibv_io_completion_service::on_poll_complete(
    void* owner, asio::detail::op_queue<rdma_verbs_op_base>& completed) {
  while (auto* op = completed.front()) {
    completed.pop();
    if (owner) {
      op->complete(owner);
    }
    else {
      op->destroy();
    }
  }
  if (owner) {
    arm_poller();
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_SERVICE_IO_COMPLETION_IPP
