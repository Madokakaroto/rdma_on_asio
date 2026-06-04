#pragma once

#include "asio/io_context.hpp"
#include "ibv/ibv_device.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_device_service.hpp"
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
  auto& dev_svc = asio::use_service<detail::ibv_device_service>(io_ctx);
  if (dev_svc.is_registered()) {
    ec = make_error_code(ibv_errc::ext_already_registered);
    return;
  }
  if (!device) {
    ec = make_error_code(ibv_errc::ext_invalid_device);
    return;
  }
  auto const effective = detail::derive_effective_config(config, device->attr_);
  // Initialize the CQ/notify service first; register the device only on success
  // so a CQ-creation failure leaves the io_context cleanly unregistered.
  auto& io_svc = asio::use_service<detail::ibv_io_completion_service>(io_ctx);
  io_svc.initialize(device, effective.cqe_, ec);
  if (ec) {
    return;
  }
  dev_svc.register_device(device, effective);
}

inline void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                       ibv_config_t const& config = {}) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}
