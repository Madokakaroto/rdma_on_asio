#pragma once

#include "asio/io_context.hpp"
#include "ibv/ibv_device.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_io_completion_service.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service on the first config-compatible
// device. Mirrors nd_use_device.
inline detail::ibv_io_completion_service& use_device(asio::io_context& io_ctx,
                                                     ibv_config_t const& config,
                                                     asio::error_code& ec) {
  auto& svc = asio::use_service<detail::ibv_io_completion_service>(io_ctx);
  if (svc.is_initialized()) {
    ec = make_error_code(ibv_errc::ext_already_registered);
    return svc;
  }

  ibv_device_ptr selected;
  auto const& mgr = ibv_device_manager_t::instance();
  mgr.for_each_device([&](ibv_device_ptr const& device) -> bool {
    if (detail::is_config_compatible(config, device->attr_)) {
      selected = device;
      return false;
    }
    return true;
  });

  if (!selected) {
    ec = make_error_code(ibv_errc::ext_invalid_device);
    return svc;
  }

  svc.initialize(selected, config, ec);
  return svc;
}

inline detail::ibv_io_completion_service& use_device(
    asio::io_context& io_ctx, ibv_config_t const& config = {}) {
  asio::error_code ec{};
  auto& svc = use_device(io_ctx, config, ec);
  asio::detail::throw_error(ec);
  return svc;
}

}
