#pragma once

#include <functional>
#include <optional>
#include "asio/io_context.hpp"
#include "nd/nd_device.hpp"
#include "nd/detail/nd_io_completion_service.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma {

using device_selector =
    std::function<std::optional<nd_config_t>(nd_device_ptr const&)>;

inline detail::nd_io_completion_service& use_device(
    asio::io_context& io_ctx, nd_config_t const& config,
    asio::error_code& ec) {
  auto& svc =
      asio::use_service<detail::nd_io_completion_service>(io_ctx);
  if (svc.is_initialized()) {
    ec = nd_errc::ext_already_registered;
    ASIO_ERROR_LOCATION(ec);
    return svc;
  }

  nd_device_ptr selected;
  auto const& mgr = nd_device_manager_t::instance();
  mgr.for_each_adapter([&](nd_device_ptr const& adapter) -> bool {
    if (detail::is_config_compatible(config, adapter->info_)) {
      selected = adapter;
      return false;
    }
    return true;
  });

  if (!selected) {
    ec = nd_errc::ext_invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return svc;
  }

  svc.initialize(selected, config, ec);
  return svc;
}

inline detail::nd_io_completion_service& use_device(
    asio::io_context& io_ctx, nd_config_t const& config = {}) {
  asio::error_code ec{};
  auto& svc = use_device(io_ctx, config, ec);
  asio::detail::throw_error(ec);
  return svc;
}

inline detail::nd_io_completion_service& use_device(
    asio::io_context& io_ctx, device_selector const& selector,
    asio::error_code& ec) {
  auto& svc =
      asio::use_service<detail::nd_io_completion_service>(io_ctx);
  if (svc.is_initialized()) {
    ec = nd_errc::ext_already_registered;
    ASIO_ERROR_LOCATION(ec);
    return svc;
  }

  nd_device_ptr selected;
  nd_config_t selected_config{};
  auto const& mgr = nd_device_manager_t::instance();
  mgr.for_each_adapter([&](nd_device_ptr const& adapter) -> bool {
    auto result = selector(adapter);
    if (result.has_value()) {
      selected = adapter;
      selected_config = result.value();
      return false;
    }
    return true;
  });

  if (!selected) {
    ec = nd_errc::ext_invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return svc;
  }

  svc.initialize(selected, selected_config, ec);
  return svc;
}

inline detail::nd_io_completion_service& use_device(
    asio::io_context& io_ctx, device_selector const& selector) {
  asio::error_code ec{};
  auto& svc = use_device(io_ctx, selector, ec);
  asio::detail::throw_error(ec);
  return svc;
}

}
