#pragma once

#include <array>
#include <cstddef>

#include "asio/detail/op_queue.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/execution_context.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_op_complete.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

namespace asio::rdma::detail {

// Per-io_context singleton (via use_service) owning a shared CQ + comp_channel
// registered to the epoll reactor, transparent to the user. Mirrors
// nd_io_completion_service (IOCP -> epoll). Created by use_device().
//
// The CQ poller (ibv_op_notify_wr) is a reused MEMBER reactor_op — no per-op
// heap allocation. It is armed only while data-plane ops are outstanding
// (tracked by pending_), so an idle io_context still drains to zero work and
// io_context::run() returns. Each posted verbs op calls arm_notify(); the
// poller drains every available completion per wakeup and dispatches them.
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
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    if (!device || !device->context_) {
      ec = make_error_code(ibv_errc::ext_invalid_device);
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

  // Called once per posted verbs op. Counts the op as outstanding and ensures
  // the (single, reused) poller is armed — unless we are mid-dispatch, in which
  // case arming is deferred to after the dispatch loop (avoids touching the
  // poller while its completion queue is being drained).
  void arm_notify() {
    ++pending_;
    if (!in_dispatch_) {
      ensure_armed();
    }
  }

private:
  static constexpr int poll_batch = 16;

  // Reused member reactor_op: drains the shared CQ on comp_channel readiness.
  // Returns not_done to stay armed until a completion is available, then done.
  class ibv_op_notify_wr : public asio::detail::reactor_op {
  public:
    explicit ibv_op_notify_wr(ibv_io_completion_service* svc)
        : asio::detail::reactor_op(asio::error_code{}, &do_perform, &do_complete)
        , svc_(svc) {
    }

  private:
    friend class ibv_io_completion_service;

    static status do_perform(asio::detail::reactor_op* base) {
      auto* o = static_cast<ibv_op_notify_wr*>(base);
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
      auto* o = static_cast<ibv_op_notify_wr*>(base);
      o->svc_->on_notify_complete(owner, o->completed_);
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

  // Arm the (single, reused) poller if not already armed. Uses NON-speculative
  // start_op so it is never performed re-entrantly from within its own
  // do_complete. To close the post-before-notify race (a WR completing between
  // ibv_post_* and ibv_req_notify_cq on fast loopback), we req_notify then poll
  // immediately; if completions are already present we enqueue the poller's
  // completion directly (post_immediate_completion only queues — not re-entrant).
  void ensure_armed() {
    if (armed_) {
      return;
    }
    armed_ = true;
    asio::error_code ec;
    verbs_ops::req_notify_cq(cq_.get(), false, ec);
    poll_into(notify_op_.completed_);
    if (!notify_op_.completed_.empty()) {
      scheduler_.post_immediate_completion(&notify_op_, false);
    }
    else {
      reactor_.start_op(asio::detail::reactor::read_op, comp_channel_->fd,
                        cq_reactor_data_, &notify_op_, false, false);
    }
  }

  // Invoked from the poller's do_complete: dispatch drained ops, then re-arm if
  // any posted ops are still outstanding. owner == nullptr means reactor
  // shutdown — free handlers without an upcall and do not re-arm.
  void on_notify_complete(void* owner,
                          asio::detail::op_queue<rdma_verbs_op_base>& completed) {
    armed_ = false;
    in_dispatch_ = true;
    while (auto* op = completed.front()) {
      completed.pop();
      if (pending_ > 0) {
        --pending_;
      }
      if (owner) {
        op->complete(owner);  // handler may post + arm_notify (deferred)
      }
      else {
        op->destroy();
      }
    }
    in_dispatch_ = false;
    if (owner && pending_ > 0) {
      ensure_armed();
    }
  }

  asio::detail::reactor& reactor_;
  asio::detail::scheduler& scheduler_;
  unique_ibv_cq_ptr cq_;
  unique_ibv_comp_channel_ptr comp_channel_;
  asio::detail::reactor::per_descriptor_data cq_reactor_data_{};
  ibv_op_notify_wr notify_op_{this};
  std::size_t pending_ = 0;
  bool armed_ = false;
  bool in_dispatch_ = false;
};

}
