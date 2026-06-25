#ifndef RDMA_IBV_IMPL_IBV_USE_DEVICE_IPP
#define RDMA_IBV_IMPL_IBV_USE_DEVICE_IPP

#include "rdma/ibv/ibv_use_device.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                ibv_config_t const& config, asio::error_code& ec) {
  auto& dev_svc = asio::use_service<detail::ibv_device_service>(io_ctx);
  if (dev_svc.is_registered()) {
    ec = make_error_code(rdma_errc::already_registered);
    return;
  }
  if (!device) {
    ec = make_error_code(rdma_errc::invalid_device);
    return;
  }
  auto const effective = detail::derive_effective_config(config, device->attr_);
  // Initialize the CQ/notify service first; register the device only on success
  // so a CQ-creation failure leaves the io_context cleanly unregistered.
  auto& io_svc = asio::use_service<detail::ibv_io_completion_service>(io_ctx);
  io_svc.initialize(device, effective.cqe_, effective.cq_poll_batch_, ec);
  if (ec) {
    return;
  }
  dev_svc.register_device(device, effective);
}

void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                ibv_config_t const& config) {
  asio::error_code ec{};
  use_device(io_ctx, device, config, ec);
  asio::detail::throw_error(ec);
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_USE_DEVICE_IPP
