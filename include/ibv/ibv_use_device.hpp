#pragma once

#include "asio/io_context.hpp"
#include "ibv/ibv_device.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/detail/ibv_io_completion_service.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service for an explicit device. The
// caller discovers the device beforehand via
// ibv_device_manager_t::instance().get_first_available_device(ps, config).
//
// Returns void: the caller already holds the device_ptr (use it directly for
// MR / completion_queue / etc.). The same device_ptr may be passed to use_device
// on multiple io_contexts — each gets its own shared CQ + comp_channel bound to
// its own reactor, all sharing the device's context/PD. Mirrors nd use_device.
inline void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                       ibv_config_t const& config, asio::error_code& ec) {
  auto& svc = asio::use_service<detail::ibv_io_completion_service>(io_ctx);
  if (svc.is_initialized()) {
    ec = make_error_code(ibv_errc::ext_already_registered);
    return;
  }
  if (!device) {
    ec = make_error_code(ibv_errc::ext_invalid_device);
    return;
  }
  svc.initialize(device, config, ec);
}

inline void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                       ibv_config_t const& config = {}) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}
