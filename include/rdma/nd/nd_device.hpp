#pragma once

#include <ranges>
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/throw_error.hpp"
#include "rdma/nd/detail/nd_asio_manual_init.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/detail/nd_device_impl.hpp"

namespace asio::rdma {

using nd_device_t = detail::nd_adapter_t;
using nd_device_ptr = detail::nd_adapter_ptr;

class nd_device_manager_t {
 private:
  detail::nd_global_t global_;
  std::vector<detail::nd_provider_ptr> providers_;

  nd_device_manager_t()
    : global_()
    , providers_(detail::get_providers()) {
    detail::open_adapters(providers_);
  }

 public:
  static nd_device_manager_t const& instance() {
    static nd_device_manager_t instance{};
    return instance;
  }

  // Return the first device whose capabilities satisfy the (non-zero) config
  // constraints. A device may carry v4 and/or v6 local addresses; callers select
  // a concrete address with get_v4_address() / get_v6_address().
  nd_device_ptr get_first_available_device(nd_config_t const& config = {}) const {
    for (auto const& provider : providers_) {
      assert(provider);
      for (auto const& device : provider->devices_) {
        if (detail::is_valid_adapter(device, config)) {
          return device;
        }
      }
    }
    return nullptr;
  }

  template <typename Func>
  void for_each_device(Func&& func) const {
    for (auto const& provider : providers_) {
      assert(provider);
      for (auto const& device : provider->devices_) {
        if (!func(device)) return;
      }
    }
  }

 private:

};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/impl/nd_device.ipp"
#endif
