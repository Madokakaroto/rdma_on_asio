#pragma once

#include <cstring>
#include <vector>

#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <rdma/rdma_cma.h>  // rdma_get_devices / rdma_free_devices

#include "asio.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/ip/tcp.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Build one ibv_device_t from a pre-opened context: alloc PD, query caps, name.
ASIO_DECL ibv_device_ptr create_device(native_context_t* context,
                                       asio::error_code& ec);

ASIO_DECL ibv_device_ptr create_device(native_context_t* context);

ASIO_DECL bool is_valid_device(ibv_device_ptr const& device);

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

ASIO_DECL std::size_t sockaddr_size(sockaddr const* addr) noexcept;

ASIO_DECL std::optional<asio::ip::address> to_ip_address(sockaddr const* addr);

ASIO_DECL bool is_candidate_local_address(ifaddrs const& interface) noexcept;

ASIO_DECL native_context_t* bindable_context_for(sockaddr const* addr);

ASIO_DECL void attach_device_address(ibv_device_ptr const& device,
                                     asio::ip::address const& address);

ASIO_DECL void attach_local_addresses(std::vector<ibv_device_ptr>& devices,
                                      asio::error_code& ec);

ASIO_DECL bool has_local_address(ibv_device_ptr const& device) noexcept;

// Discover all RDMA devices via librdmacm. The returned contexts are the ones
// rdma_cm reuses internally, so the PDs allocated here are usable with
// rdma_create_qp(cm_id, pd, ...) later.
//
// rdma_get_devices() returns an array of pointers to contexts that librdmacm
// owns internally; the contexts stay valid for the process lifetime. Only the
// array itself must be released (rdma_free_devices just frees the array), so we
// copy the pointers into the device list and free the array before returning.
ASIO_DECL std::vector<ibv_device_ptr> get_devices(asio::error_code& ec);

ASIO_DECL std::vector<ibv_device_ptr> get_devices();

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_device_impl.ipp"
#endif
