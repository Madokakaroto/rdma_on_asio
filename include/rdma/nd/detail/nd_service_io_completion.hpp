#pragma once

#include <array>
#include <atomic>
#include <ranges>
#include <vector>

#include "asio/detail/config.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/op_queue.hpp"
#include "asio/execution_context.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/detail/nd_config_derive.hpp"
#include "rdma/nd/detail/nd_impl_types.hpp"
#include "rdma/nd/detail/nd_device_impl.hpp"
#include "rdma/nd/detail/nd_op_base.hpp"
#include "rdma/nd/detail/nd_ops_verbs.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"

namespace asio::rdma::detail {

// Per-io_context singleton owning a shared CQ + IOCP overlapped handle. The CQ
// poller is started lazily when the first event-mode queue_pair binds
// (ensure_poller_started()).
//
// Thread-safe & lock-free: the poller is a single self-perpetuating op (a fresh
// IOCP Notify each cycle, but only ONE outstanding at a time), so the CQ
// GetResults is serialized across run() threads with no lock; submitter threads
// only post on their QP and touch no service state.
//
// Consequence (mirrors ibv): once started, the poller is outstanding IOCP work
// for the io_context's lifetime, so io_context::run() no longer returns merely
// because the data plane is idle -- stop via io_context::stop() / destruction.
// poll-mode-only / control-plane-only io_contexts never start it.
class nd_io_completion_service
    : public asio::detail::execution_context_service_base<
          nd_io_completion_service> {
public:
  using base_type =
      asio::detail::execution_context_service_base<nd_io_completion_service>;

  explicit nd_io_completion_service(asio::execution_context& ctx)
      : base_type(ctx)
      , scheduler_(asio::use_service<asio::detail::win_iocp_io_context>(ctx)) {
  }

  ~nd_io_completion_service() = default;

  ASIO_DECL void shutdown() override;

  // Create the shared CQ + overlapped handle on the IOCP scheduler. The device is
  // used transiently for its adapter (not stored); cqe is the derived CQ depth.
  ASIO_DECL void initialize(nd_adapter_ptr const& device, std::uint32_t cqe,
                            std::uint32_t poll_batch, asio::error_code& ec);

  native_cq_t* get_cq() const noexcept { return cq_.Get(); }

  // Start the self-perpetuating CQ poller. Idempotent + thread-safe: the first
  // event-mode queue_pair to bind fires it; after that it re-arms itself forever.
  ASIO_DECL void ensure_poller_started();

private:
  // Single-in-flight, self-perpetuating CQ poller. Each cycle uses a fresh IOCP
  // overlapped op (allocated in arm_poller), so only one is ever outstanding --
  // GetResults is serialized. On completion it drains+dispatches the CQ and
  // re-arms; owner == nullptr (io_context shutdown) frees without re-arming.
  class nd_poll_wc_op final : public asio::detail::operation {
  public:
    using base_type = asio::detail::operation;
    struct Handler {};
    ASIO_DEFINE_HANDLER_PTR(nd_poll_wc_op);

    explicit nd_poll_wc_op(nd_io_completion_service* svc)
        : base_type(&nd_poll_wc_op::do_complete), svc_(svc) {
    }

  private:
    ASIO_DECL static void do_complete(void* owner, base_type* base,
                                      asio::error_code const& result_ec,
                                      std::size_t bytes_transferred);

    nd_io_completion_service* svc_;
  };

  ASIO_DECL static rdma_verbs_op_base* resolve_wc(native_wc_t const& result);

  ASIO_DECL void poll_and_dispatch(void* owner);

  // Arm (or re-arm) the single poller: allocate a fresh overlapped op and request
  // an IOCP completion notification for the next CQ event.
  ASIO_DECL void arm_poller();

  asio::detail::win_iocp_io_context& scheduler_;
  nd2_completion_queue_ptr cq_;
  unique_handle_t cq_handle_;
  std::vector<native_wc_t> wc_buf_;
  std::atomic<bool> poller_started_{false};
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_service_io_completion.ipp"
#endif
