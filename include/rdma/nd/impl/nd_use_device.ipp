#ifndef RDMA_ND_IMPL_ND_USE_DEVICE_IPP
#define RDMA_ND_IMPL_ND_USE_DEVICE_IPP

#include "rdma/nd/nd_use_device.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
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
  io_svc.initialize(device, effective.cqe_, effective.cq_poll_batch_, ec);
  if (ec) {
    return;
  }
  dev_svc.register_device(device, effective);
}

void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                nd_config_t const& config) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_USE_DEVICE_IPP
