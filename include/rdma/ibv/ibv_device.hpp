#pragma once

#include <vector>

#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/detail/ibv_config_derive.hpp"
#include "rdma/ibv/detail/ibv_device_impl.hpp"

namespace asio::rdma {

using ibv_device_t = detail::ibv_device_t;
using ibv_device_ptr = detail::ibv_device_ptr;

// Process-wide, const-immutable device registry (mirrors nd_device_manager_t).
// Discovery runs once at first access; the device list is then immutable.
class ibv_device_manager_t {
 private:
  std::vector<ibv_device_ptr> devices_;

  // get_devices() copies the librdmacm-owned context pointers into devices_ and
  // frees the rdma_get_devices() array internally; the contexts stay valid for
  // the process lifetime, so the manager only needs to hold the device list.
  // PDs are released by the unique_ibv_pd_ptr deleters when devices_ is destroyed.
  ibv_device_manager_t() : devices_(detail::get_devices()) {}  // throws if none

 public:
  static ibv_device_manager_t const& instance() {
    static ibv_device_manager_t instance{};
    return instance;
  }

  // The port space is accepted for signature parity with the nd backend. Verbs
  // devices are not bound to a v4/v6 address family at this layer (that happens
  // at rdma_cm connect time), so family is ignored and the first config-compatible
  // device is returned.
  template <typename PortSpace>
  ibv_device_ptr get_first_available_device(PortSpace const& /*ps*/,
                                            ibv_config_t const& config) const {
    for (auto const& device : devices_) {
      if (detail::is_valid_device(device) &&
          detail::is_config_compatible(config, device->attr_)) {
        return device;
      }
    }
    return nullptr;
  }

  // func(ibv_device_ptr const&) -> bool; return false to stop iteration.
  template <typename Func>
  void for_each_device(Func&& func) const {
    for (auto const& device : devices_) {
      if (!func(device)) {
        return;
      }
    }
  }

  ibv_device_manager_t(ibv_device_manager_t const&) = delete;
  ibv_device_manager_t& operator=(ibv_device_manager_t const&) = delete;
};

}
