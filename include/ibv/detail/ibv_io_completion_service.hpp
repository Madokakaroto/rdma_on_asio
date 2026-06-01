#pragma once

#include "asio/detail/reactor.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/execution_context.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_op_cq_notify.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

namespace asio::rdma::detail {

// Per-io_context singleton (via use_service) owning a shared CQ + comp_channel
// registered to the epoll reactor, transparent to the user. Mirrors
// nd_io_completion_service (IOCP -> epoll). Created by use_device().
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
      reactor_.deregister_descriptor(comp_channel_->fd, cq_reactor_data_,
                                     false);
      reactor_.cleanup_descriptor_data(cq_reactor_data_);
    }
    cq_.reset();
    comp_channel_.reset();
    device_.reset();
    initialized_ = false;
  }

  void initialize(ibv_device_ptr const& device, ibv_config_t const& config,
                  asio::error_code& ec) {
    if (initialized_) {
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    if (!device || !device->context_) {
      ec = make_error_code(ibv_errc::ext_invalid_device);
      return;
    }

    effective_config_ = derive_effective_config(config, device->attr_);

    comp_channel_.reset(
        verbs_ops::create_comp_channel(device->context_, ec));
    if (ec) {
      return;
    }
    cq_.reset(verbs_ops::create_cq(device->context_,
                                   static_cast<int>(effective_config_.cqe_),
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

    device_ = device;
    initialized_ = true;
    ec.clear();
  }

  bool is_initialized() const noexcept { return initialized_; }
  ibv_device_ptr get_device() const noexcept { return device_; }
  native_cq_t* get_cq() const noexcept { return cq_.get(); }
  native_comp_channel_t* get_comp_channel() const noexcept {
    return comp_channel_.get();
  }
  ibv_config_t const& get_effective_config() const noexcept {
    return effective_config_;
  }

  // Arm a CQ poller for one posted verbs op (per-op arming, like nd).
  void arm_notify() {
    ibv_cq_notify_op::arm(asio::error_code{}, reactor_, cq_reactor_data_,
                          comp_channel_.get(), cq_.get());
  }

private:
  asio::detail::reactor& reactor_;
  asio::detail::scheduler& scheduler_;
  ibv_device_ptr device_;
  ibv_config_t effective_config_{};
  unique_ibv_cq_ptr cq_;
  unique_ibv_comp_channel_ptr comp_channel_;
  asio::detail::reactor::per_descriptor_data cq_reactor_data_{};
  bool initialized_ = false;
};

}
