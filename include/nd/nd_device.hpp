#pragma once

#include <ranges>
#include "nd/detail/nd_asio_manual_init.hpp"
#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"
#include "nd/detail/nd_device_impl.hpp"

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

  template <typename PortSpace>
  nd_device_ptr get_first_available_device(PortSpace const& ps,
                                           nd_config_t const& config) const {
    for (auto const& provider : providers_) {
      assert(provider);
      auto const& adapters = ps.get_adapters(*provider);
      for (auto adapter : adapters) {
        if (detail::is_valid_adapter(adapter, config)) {
          return adapter;
        }
      }
    }
    return nullptr;
  }

  template <typename Func>
  void for_each_adapter(Func&& func) const {
    for (auto const& provider : providers_) {
      assert(provider);
      for (auto const& adapter : provider->v4_adapters_) {
        if (!func(adapter)) return;
      }
      for (auto const& adapter : provider->v6_adapters_) {
        if (!func(adapter)) return;
      }
    }
  }

 private:

};

}