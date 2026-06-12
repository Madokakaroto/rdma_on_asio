#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "asio/detail/op_queue.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/execution_context.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_op_complete.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

namespace asio::rdma::detail {

// Per-io_context singleton (via use_service) owning a shared CQ + comp_channel
// registered to the epoll reactor. Created by use_device(); the CQ poller is
// started lazily when the first event-mode queue_pair binds to this io_context
// (ensure_poller_started()).
//
// Thread-safe & lock-free: the poller (ibv_poll_wc_op) is a single reused MEMBER
// reactor_op, fired exactly once (one-time atomic) and then re-arming itself
// unconditionally after every dispatch. It is the ONLY thing that touches the CQ
// / reactor for the data plane; submitter threads only ibv_post_* on their QP and
// touch no service state. asio never performs a single op concurrently, so the
// poller (hence ibv_poll_cq) is serialized across multiple run() threads with no
// lock.
//
// Consequence: once started, the poller is an outstanding reactor op for the
// io_context's lifetime, so io_context::run() no longer returns merely because the
// data plane is idle — stop via io_context::stop() / destruction (shutdown()
// cancels the poller). An io_context that never binds an event-mode QP
// (poll-mode-only / control-plane-only) never starts the poller and keeps the
// usual "run() returns when idle" behavior.
class ibv_io_completion_service
    : public asio::detail::execution_context_service_base<
          ibv_io_completion_service> {
public:
  using base_type = asio::detail::execution_context_service_base<
      ibv_io_completion_service>;

  explicit ibv_io_completion_service(asio::execution_context& ctx)
      : base_type(ctx)
      , reactor_(asio::use_service<asio::detail::reactor>(ctx))
      , scheduler_(asio::use_service<asio::detail::scheduler>(ctx)) {
    reactor_.init_task();  // install the epoll task in the scheduler
  }

  void shutdown() override {
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
  // transiently for its context (not stored — that's ibv_device_service's job);
  // cqe is the already-derived CQ depth. Idempotency self-guards on cq_.
  void initialize(ibv_device_ptr const& device, std::uint32_t cqe,
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
    ec.clear();
  }

  native_cq_t* get_cq() const noexcept { return cq_.get(); }
  native_comp_channel_t* get_comp_channel() const noexcept {
    return comp_channel_.get();
  }

  // Start the self-perpetuating CQ poller. Idempotent + thread-safe: the first
  // caller (the first event-mode queue_pair to bind on this io_context) fires it;
  // everyone else is a no-op. After this the poller re-arms itself forever, so no
  // submitter ever touches the CQ/reactor again.
  void ensure_poller_started() {
    if (!poller_started_.exchange(true, std::memory_order_acq_rel)) {
      arm_poller();
    }
  }

private:
  static constexpr int poll_batch = 16;

  // Single reused member reactor_op: drains the shared CQ on comp_channel
  // readiness. Returns not_done (spurious wake) to stay armed, or done to
  // dispatch what it drained.
  class ibv_poll_wc_op : public asio::detail::reactor_op {
  public:
    explicit ibv_poll_wc_op(ibv_io_completion_service* svc)
        : asio::detail::reactor_op(asio::error_code{}, &do_perform, &do_complete)
        , svc_(svc) {
    }

  private:
    friend class ibv_io_completion_service;

    static status do_perform(asio::detail::reactor_op* base) {
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

    static void do_complete(void* owner, asio::detail::operation* base,
                            asio::error_code const& /*result_ec*/,
                            std::size_t /*bytes_transferred*/) {
      auto* o = static_cast<ibv_poll_wc_op*>(base);
      o->svc_->on_poll_complete(owner, o->completed_);
    }

    ibv_io_completion_service* svc_;
    asio::detail::op_queue<rdma_verbs_op_base> completed_;
  };

  // Drain the CQ, resolving each work completion to its verbs op.
  void poll_into(asio::detail::op_queue<rdma_verbs_op_base>& out) {
    int n = 0;
    do {
      std::array<native_wc_t, poll_batch> wcs{};
      n = verbs_ops::poll_cq(cq_.get(), poll_batch, wcs.data());
      for (int i = 0; i < n; ++i) {
        if (auto* op = resolve_verbs_op(wcs[i])) {
          out.push(op);
        }
      }
    } while (n > 0);
  }

  // Arm (or re-arm) the single poller. To close the post-before-notify race (a WR
  // completing between ibv_post_* and ibv_req_notify_cq on fast loopback) we
  // req_notify then poll immediately; if completions are already present we queue
  // the poller's completion directly, else we wait on the comp_channel fd. Only
  // ever called from ensure_poller_started() (one-time) or on_poll_complete()
  // (after the poller's own dispatch), so it is never run concurrently.
  void arm_poller() {
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
  // owner == nullptr means reactor shutdown — free handlers without an upcall and
  // do not re-arm.
  void on_poll_complete(void* owner,
                        asio::detail::op_queue<rdma_verbs_op_base>& completed) {
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

  asio::detail::reactor& reactor_;
  asio::detail::scheduler& scheduler_;
  unique_ibv_cq_ptr cq_;
  unique_ibv_comp_channel_ptr comp_channel_;
  asio::detail::reactor::per_descriptor_data cq_reactor_data_{};
  ibv_poll_wc_op poller_{this};
  std::atomic<bool> poller_started_{false};
};

}
