#pragma once

#include "asio/execution_context.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Per-io_context registration of the device + effective (operating) config that
// use_device selected. Single responsibility: hold {device, effective_config}.
//
// Distinct from the process-wide ibv_device_manager_t (which does device
// *discovery*) --this service is the per-io_context binding, and is the single
// source of truth for "use_device was called on this io_context"
// (is_registered()). The connector/listener/queue_pair read device + config and
// the registration guard from here; the CQ/notify mechanism lives in the
// separate ibv_io_completion_service.
class ibv_device_service
    : public asio::detail::execution_context_service_base<ibv_device_service> {
public:
  using base_type =
      asio::detail::execution_context_service_base<ibv_device_service>;

  explicit ibv_device_service(asio::execution_context& ctx) : base_type(ctx) {}

  void shutdown() override {
    device_.reset();
    registered_ = false;
  }

  // Store the device + already-derived effective config (called by use_device
  // after the io_completion_service CQ is successfully initialized).
  void register_device(ibv_device_ptr const& device,
                       ibv_config_t const& effective) {
    device_ = device;
    effective_config_ = effective;
    registered_ = true;
  }

  bool is_registered() const noexcept { return registered_; }
  ibv_device_ptr get_device() const noexcept { return device_; }
  ibv_config_t const& get_effective_config() const noexcept {
    return effective_config_;
  }

private:
  ibv_device_ptr device_;
  ibv_config_t effective_config_{};
  bool registered_ = false;
};

}
