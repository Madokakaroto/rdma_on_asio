#pragma once

#include <cstring>
#include <vector>

#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <rdma/rdma_cma.h>  // rdma_get_devices / rdma_free_devices

#include "asio.hpp"
#include "asio/ip/tcp.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Build one ibv_device_t from a pre-opened context: alloc PD, query caps, name.
inline ibv_device_ptr create_device(native_context_t* context,
                                    asio::error_code& ec) {
  if (!context) {
    ec = make_error_code(rdma_errc::invalid_device);
    return nullptr;
  }

  auto result = std::make_shared<ibv_device_t>();
  result->context_ = context;

  unique_ibv_pd_ptr pd{ ::ibv_alloc_pd(context) };
  if (!pd) {
    ec = last_system_error();
    return nullptr;
  }

  if (::ibv_query_device(context, &result->attr_) != 0) {
    ec = last_system_error();
    return nullptr;
  }

  result->pd_ = std::move(pd);
  if (char const* name = ::ibv_get_device_name(context->device)) {
    result->name_ = name;
  }
  ec.clear();
  return result;
}

inline ibv_device_ptr create_device(native_context_t* context) {
  asio::error_code ec{};
  auto device = create_device(context, ec);
  throw_error(ec);
  return device;
}

inline bool is_valid_device(ibv_device_ptr const& device) {
  return device && device->context_ != nullptr;
}

struct ifaddrs_list {
  ifaddrs* head = nullptr;

  explicit ifaddrs_list(asio::error_code& ec) {
    if (::getifaddrs(&head) != 0) {
      ec = last_system_error();
    } else {
      ec.clear();
    }
  }

  ~ifaddrs_list() {
    if (head) {
      ::freeifaddrs(head);
    }
  }

  ifaddrs_list(ifaddrs_list const&) = delete;
  ifaddrs_list& operator=(ifaddrs_list const&) = delete;
};

inline std::size_t sockaddr_size(sockaddr const* addr) noexcept {
  if (!addr) {
    return 0;
  }
  if (addr->sa_family == AF_INET) {
    return sizeof(sockaddr_in);
  }
  if (addr->sa_family == AF_INET6) {
    return sizeof(sockaddr_in6);
  }
  return 0;
}

inline std::optional<asio::ip::address> to_ip_address(sockaddr const* addr) {
  auto const size = sockaddr_size(addr);
  if (size == 0) {
    return std::nullopt;
  }

  asio::ip::tcp::endpoint endpoint;
  std::memcpy(endpoint.data(), addr, size);
  endpoint.resize(size);
  return endpoint.address();
}

inline bool is_candidate_local_address(ifaddrs const& interface) noexcept {
  if (!interface.ifa_addr) {
    return false;
  }
  if ((interface.ifa_flags & IFF_UP) == 0 ||
      (interface.ifa_flags & IFF_LOOPBACK) != 0) {
    return false;
  }
  return sockaddr_size(interface.ifa_addr) != 0;
}

inline native_context_t* bindable_context_for(sockaddr const* addr) {
  native_cm_id_t* raw_id = nullptr;
  if (::rdma_create_id(nullptr, &raw_id, nullptr, RDMA_PS_TCP) != 0) {
    return nullptr;
  }
  cm_id_holder id{raw_id};
  if (::rdma_bind_addr(id.get(), const_cast<sockaddr*>(addr)) != 0) {
    return nullptr;
  }
  return id->verbs;
}

inline void attach_device_address(ibv_device_ptr const& device,
                                  asio::ip::address const& address) {
  if (address.is_v4()) {
    if (!device->v4_address_) {
      device->v4_address_ = address;
    }
  } else if (address.is_v6()) {
    if (!device->v6_address_) {
      device->v6_address_ = address;
    }
  }
}

inline void attach_local_addresses(std::vector<ibv_device_ptr>& devices,
                                   asio::error_code& ec) {
  ifaddrs_list interfaces{ec};
  if (ec) {
    return;
  }

  for (auto* it = interfaces.head; it; it = it->ifa_next) {
    if (!is_candidate_local_address(*it)) {
      continue;
    }
    auto address = to_ip_address(it->ifa_addr);
    if (!address) {
      continue;
    }
    auto* context = bindable_context_for(it->ifa_addr);
    if (!context) {
      continue;
    }
    for (auto const& device : devices) {
      if (device && device->context_ == context) {
        attach_device_address(device, *address);
        break;
      }
    }
  }

  ec.clear();
}

inline bool has_local_address(ibv_device_ptr const& device) noexcept {
  return device && (device->v4_address_ || device->v6_address_);
}

// Discover all RDMA devices via librdmacm. The returned contexts are the ones
// rdma_cm reuses internally, so the PDs allocated here are usable with
// rdma_create_qp(cm_id, pd, ...) later.
//
// rdma_get_devices() returns an array of pointers to contexts that librdmacm
// owns internally; the contexts stay valid for the process lifetime. Only the
// array itself must be released (rdma_free_devices just frees the array), so we
// copy the pointers into the device list and free the array before returning.
inline std::vector<ibv_device_ptr> get_devices(asio::error_code& ec) {
  std::vector<ibv_device_ptr> devices;

  int count = 0;
  native_context_t** array = ::rdma_get_devices(&count);
  if (!array || count <= 0) {
    ec = make_error_code(rdma_errc::no_available_device);
    if (array) {
      ::rdma_free_devices(array);
    }
    return devices;
  }

  devices.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    asio::error_code dev_ec{};
    auto device = create_device(array[i], dev_ec);
    if (dev_ec || !device) {
      continue;  // skip an unusable device, mirroring nd's provider/adapter filter
    }
    devices.push_back(std::move(device));
  }

  // The contexts remain valid after this; only the array is freed.
  ::rdma_free_devices(array);

  attach_local_addresses(devices, ec);
  if (ec) {
    return {};
  }

  std::vector<ibv_device_ptr> addressable_devices;
  addressable_devices.reserve(devices.size());
  for (auto& device : devices) {
    if (has_local_address(device)) {
      addressable_devices.push_back(std::move(device));
    }
  }
  devices = std::move(addressable_devices);

  if (devices.empty()) {
    ec = make_error_code(rdma_errc::no_available_device);
  }
  else {
    ec.clear();
  }
  return devices;
}

inline std::vector<ibv_device_ptr> get_devices() {
  asio::error_code ec{};
  auto devices = get_devices(ec);
  throw_error(ec);
  return devices;
}

}
