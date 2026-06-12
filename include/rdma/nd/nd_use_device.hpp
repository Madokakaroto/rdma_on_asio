#pragma once

#include "asio/io_context.hpp"
#include "rdma/nd/nd_device.hpp"
#include "rdma/nd/detail/nd_config_derive.hpp"
#include "rdma/nd/detail/nd_service_device.hpp"
#include "rdma/nd/detail/nd_service_io_completion.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service for an explicit device. The
// caller discovers the device beforehand via
// nd_device_manager_t::instance().get_first_available_device(ps, config).
//
// Returns void: the caller already holds the device_ptr. The same device_ptr may
// be passed to use_device on multiple io_contexts. Mirrors ibv use_device.
inline void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                       nd_config_t const& config, asio::error_code& ec) {
  auto& dev_svc = asio::use_service<detail::nd_device_service>(io_ctx);
  if (dev_svc.is_registered()) {
    ec = rdma_errc::already_registered;
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  if (!device) {
    ec = rdma_errc::invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  auto const effective = detail::derive_effective_config(config, device->info_);
  // Initialize the CQ/notify service first; register the device only on success.
  auto& io_svc = asio::use_service<detail::nd_io_completion_service>(io_ctx);
  io_svc.initialize(device, effective.cqe_, ec);
  if (ec) {
    return;
  }
  dev_svc.register_device(device, effective);
}

inline void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                       nd_config_t const& config = {}) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}
