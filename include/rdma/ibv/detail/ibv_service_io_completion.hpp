#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/op_queue.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/execution_context.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"
#include "rdma/ibv/detail/ibv_op_complete.hpp"
#include "rdma/ibv/detail/ibv_ops_verbs.hpp"

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
// data plane is idle --stop via io_context::stop() / destruction (shutdown()
// cancels the poller). An io_context that never binds an event-mode QP
// (poll-mode-only / control-plane-only) never starts the poller and keeps the
// usual "run() returns when idle" behavior.
class ibv_io_completion_service
    : public asio::detail::execution_context_service_base<
          ibv_io_completion_service> {
public:
  using base_type = asio::detail::execution_context_service_base<
      ibv_io_completion_service>;

  ASIO_DECL explicit ibv_io_completion_service(asio::execution_context& ctx);

  ASIO_DECL void shutdown() override;

  // Create the shared CQ + comp_channel on the reactor. The device is used
  // transiently for its context (not stored --that's ibv_device_service's job);
  // cqe is the already-derived CQ depth. Idempotency self-guards on cq_.
  ASIO_DECL void initialize(ibv_device_ptr const& device, std::uint32_t cqe,
                            std::uint32_t poll_batch, asio::error_code& ec);

  native_cq_t* get_cq() const noexcept { return cq_.get(); }
  native_comp_channel_t* get_comp_channel() const noexcept {
    return comp_channel_.get();
  }

  // Start the self-perpetuating CQ poller. Idempotent + thread-safe: the first
  // caller (the first event-mode queue_pair to bind on this io_context) fires it;
  // everyone else is a no-op. After this the poller re-arms itself forever, so no
  // submitter ever touches the CQ/reactor again.
  ASIO_DECL void ensure_poller_started();

private:

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

    ASIO_DECL static status do_perform(asio::detail::reactor_op* base);

    ASIO_DECL static void do_complete(void* owner,
                                      asio::detail::operation* base,
                                      asio::error_code const& result_ec,
                                      std::size_t bytes_transferred);

    ibv_io_completion_service* svc_;
    asio::detail::op_queue<rdma_verbs_op_base> completed_;
  };

  // Drain at most a bounded number of CQ batches, resolving each work
  // completion to its verbs op.
  ASIO_DECL void poll_into(asio::detail::op_queue<rdma_verbs_op_base>& out);

  // Arm (or re-arm) the single poller. To close the post-before-notify race (a WR
  // completing between ibv_post_* and ibv_req_notify_cq on fast loopback) we
  // req_notify then poll immediately; if completions are already present we queue
  // the poller's completion directly, else we wait on the comp_channel fd. Only
  // ever called from ensure_poller_started() (one-time) or on_poll_complete()
  // (after the poller's own dispatch), so it is never run concurrently.
  ASIO_DECL void arm_poller();

  // Poller completion: dispatch the drained ops, then re-arm unconditionally.
  // owner == nullptr means reactor shutdown --free handlers without an upcall and
  // do not re-arm.
  ASIO_DECL void on_poll_complete(
      void* owner, asio::detail::op_queue<rdma_verbs_op_base>& completed);

  asio::detail::reactor& reactor_;
  asio::detail::scheduler& scheduler_;
  int poll_batch_ = 16;
  std::vector<native_wc_t> wc_buf_;
  unique_ibv_cq_ptr cq_;
  unique_ibv_comp_channel_ptr comp_channel_;
  asio::detail::reactor::per_descriptor_data cq_reactor_data_{};
  ibv_poll_wc_op poller_{this};
  std::atomic<bool> poller_started_{false};
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_service_io_completion.ipp"
#endif
