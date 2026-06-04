#pragma once

#include "asio/io_context.hpp"
#include "nd/nd_device.hpp"
#include "nd/detail/nd_io_completion_service.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service for an explicit device. The
// caller discovers the device beforehand via
// nd_device_manager_t::instance().get_first_available_device(ps, config).
//
// Returns void: the caller already holds the device_ptr. The same device_ptr may
// be passed to use_device on multiple io_contexts. Mirrors ibv use_device.
inline void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                       nd_config_t const& config, asio::error_code& ec) {
  auto& svc = asio::use_service<detail::nd_io_completion_service>(io_ctx);
  if (svc.is_initialized()) {
    ec = nd_errc::ext_already_registered;
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  if (!device) {
    ec = nd_errc::ext_invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  svc.initialize(device, config, ec);
}

inline void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                       nd_config_t const& config = {}) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}
