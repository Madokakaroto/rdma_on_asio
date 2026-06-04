#pragma once

#include "asio/execution_context.hpp"
#include "nd/nd_types.hpp"
#include "nd/detail/nd_impl_types.hpp"

namespace asio::rdma::detail {

// Per-io_context registration of the device (adapter) + effective config that
// use_device selected. Single responsibility: hold {device, effective_config}.
//
// Distinct from the process-wide nd_device_manager_t (device *discovery*) -- this
// is the per-io_context binding and the single source of truth for "use_device
// was called on this io_context" (is_registered()). The CQ/IOCP notify mechanism
// lives in the separate nd_io_completion_service.
class nd_device_service
    : public asio::detail::execution_context_service_base<nd_device_service> {
public:
  using base_type =
      asio::detail::execution_context_service_base<nd_device_service>;

  explicit nd_device_service(asio::execution_context& ctx) : base_type(ctx) {}

  void shutdown() override {
    device_.reset();
    registered_ = false;
  }

  void register_device(nd_adapter_ptr const& device,
                       nd_config_t const& effective) {
    device_ = device;
    effective_config_ = effective;
    registered_ = true;
  }

  bool is_registered() const noexcept { return registered_; }
  nd_adapter_ptr const& get_device() const noexcept { return device_; }
  nd_config_t const& get_effective_config() const noexcept {
    return effective_config_;
  }

private:
  nd_adapter_ptr device_;
  nd_config_t effective_config_{};
  bool registered_ = false;
};

}
