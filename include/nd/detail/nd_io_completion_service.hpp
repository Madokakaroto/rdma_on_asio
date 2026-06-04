#pragma once

#include "asio/detail/config.hpp"
#include "asio/execution_context.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"
#include "nd/detail/nd_impl_types.hpp"
#include "nd/detail/nd_device_impl.hpp"
#include "nd/detail/nd_ops_verbs.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma::detail {

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

  void shutdown() override {
    cq_.Reset();
    cq_handle_.reset();
  }

  // Create the shared CQ + overlapped handle on the IOCP scheduler. The device
  // is used transiently for its adapter (not stored — that's nd_device_service's
  // job); cqe is the already-derived CQ depth. Idempotency self-guards on cq_.
  void initialize(nd_adapter_ptr const& device, std::uint32_t cqe,
                  asio::error_code& ec) {
    if (cq_) {
      ec = nd_errc::ext_already_registered;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    if (!device || !device->adapter_) {
      ec = nd_errc::ext_invalid_device;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    cq_handle_.reset(
        create_overlapped_file(device->adapter_.Get(), ec));
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    native_cq_init_attr cq_init_attr{
        .overlapped_handle_ = cq_handle_.get(),
        .processor_group_ = 0,
        .processor_affinity_ = 0,
    };
    cq_.Attach(verbs_ops::create_cq(device->adapter_.Get(), cqe, cq_init_attr,
                                    ec));
    if (ec) {
      cq_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    scheduler_.register_handle(cq_handle_.get(), ec);
    if (ec) {
      cq_.Reset();
      cq_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }
  }

  native_cq_t* get_cq() const noexcept { return cq_.Get(); }

  void arm_notify(asio::detail::operation* notify_op, asio::error_code& ec) {
    assert(cq_);
    assert(notify_op);
    native_cq_notify_attr notify_attr{
        .type_ = ND_CQ_NOTIFY_ANY,
        .op_ = notify_op,
    };
    verbs_ops::notify_cq(cq_.Get(), notify_attr, ec);

    scheduler_.work_started();

    if (!ec || ec == nd_errc::pending) {
      scheduler_.on_pending(notify_op);
      ec.clear();
      return;
    }

    scheduler_.on_completion(notify_op, ec, 0L);
  }

private:
  asio::detail::win_iocp_io_context& scheduler_;
  nd2_completion_queue_ptr cq_;
  unique_handle_t cq_handle_;
};

}
