#pragma once

#include <cstring>
#include <stdexcept>

#include "asio/ip/address.hpp"
#include "rdma/rdma_types.hpp"

#if defined(ASIO_RDMA_BACKEND_VERBS)
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

namespace asio::rdma {

namespace detail {

inline asio::ip::address make_ip_address(sockaddr const& addr) {
  if (addr.sa_family == AF_INET) {
    auto const& in = reinterpret_cast<sockaddr_in const&>(addr);
    asio::ip::address_v4::bytes_type bytes{};
    static_assert(sizeof(bytes) == sizeof(in.sin_addr));
    std::memcpy(bytes.data(), &in.sin_addr, bytes.size());
    return asio::ip::address_v4(bytes);
  }

  if (addr.sa_family == AF_INET6) {
    auto const& in6 = reinterpret_cast<sockaddr_in6 const&>(addr);
    asio::ip::address_v6::bytes_type bytes{};
    static_assert(sizeof(bytes) == sizeof(in6.sin6_addr));
    std::memcpy(bytes.data(), &in6.sin6_addr, bytes.size());
    return asio::ip::address_v6(bytes, in6.sin6_scope_id);
  }

  throw std::runtime_error("unsupported RDMA local address family");
}

}  // namespace detail

#if defined(ASIO_RDMA_BACKEND_ND)

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const& device) {
  if (!device) {
    throw std::runtime_error("no RDMA device available");
  }

  if (device->v4_addr_) {
    return detail::make_ip_address(device->v4_addr_->src_addr_);
  }
  if (device->v6_addr_) {
    return detail::make_ip_address(device->v6_addr_->src_addr_);
  }

  throw std::runtime_error("RDMA device has no local address");
}

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const& device,
                                                  tcp port_space) {
  if (!device) {
    throw std::runtime_error("no RDMA device available");
  }

  if (port_space.family() == AF_INET && device->v4_addr_) {
    return detail::make_ip_address(device->v4_addr_->src_addr_);
  }
  if (port_space.family() == AF_INET6 && device->v6_addr_) {
    return detail::make_ip_address(device->v6_addr_->src_addr_);
  }

  throw std::runtime_error("RDMA device has no local address for requested family");
}

#elif defined(ASIO_RDMA_BACKEND_VERBS)

namespace detail {

struct ifaddrs_list {
  struct ifaddrs* head = nullptr;

  ifaddrs_list() {
    if (::getifaddrs(&head) != 0) {
      throw std::runtime_error("getifaddrs failed");
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

struct rdma_cm_id_holder {
  rdma_cm_id* id = nullptr;

  ~rdma_cm_id_holder() {
    if (id) {
      ::rdma_destroy_id(id);
    }
  }

  rdma_cm_id_holder(rdma_cm_id_holder const&) = delete;
  rdma_cm_id_holder& operator=(rdma_cm_id_holder const&) = delete;
};

inline bool can_bind_rdma_address(sockaddr const* addr,
                                  native_context_t* expected_context) {
  rdma_cm_id_holder holder;
  if (::rdma_create_id(nullptr, &holder.id, nullptr, RDMA_PS_TCP) != 0) {
    return false;
  }
  if (::rdma_bind_addr(holder.id, const_cast<sockaddr*>(addr)) != 0) {
    return false;
  }
  return !expected_context || holder.id->verbs == expected_context;
}

inline asio::ip::address query_local_rdma_address_by_family(
    rdma_device_ptr const& device, int family) {
  ifaddrs_list interfaces;
  for (auto* it = interfaces.head; it; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != family) {
      continue;
    }
    if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    if (can_bind_rdma_address(it->ifa_addr, device->context_)) {
      return make_ip_address(*it->ifa_addr);
    }
  }
  throw std::runtime_error("no bindable local RDMA address found");
}

}  // namespace detail

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const& device) {
  if (!device) {
    throw std::runtime_error("no RDMA device available");
  }

  try {
    return detail::query_local_rdma_address_by_family(device, AF_INET);
  } catch (std::runtime_error const&) {
    return detail::query_local_rdma_address_by_family(device, AF_INET6);
  }
}

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const& device,
                                                  tcp port_space) {
  if (!device) {
    throw std::runtime_error("no RDMA device available");
  }
  return detail::query_local_rdma_address_by_family(device, port_space.family());
}

#else

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const&) {
  throw std::runtime_error("no RDMA backend selected");
}

inline asio::ip::address query_local_rdma_address(rdma_device_ptr const&,
                                                  tcp) {
  throw std::runtime_error("no RDMA backend selected");
}

#endif

inline asio::ip::address query_local_rdma_address() {
  auto device = rdma_device_manager_t::instance().get_first_available_device({});
  return query_local_rdma_address(device);
}

inline asio::ip::address query_local_rdma_address(tcp port_space) {
  auto device = rdma_device_manager_t::instance().get_first_available_device({});
  return query_local_rdma_address(device, port_space);
}

}  // namespace asio::rdma
