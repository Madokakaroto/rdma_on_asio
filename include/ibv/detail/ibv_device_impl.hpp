#pragma once

#include <vector>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>  // rdma_get_devices / rdma_free_devices

#include "asio.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Build one ibv_device_t from a pre-opened context: alloc PD, query caps, name.
inline ibv_device_ptr create_device(native_context_t* context,
                                    asio::error_code& ec) {
  if (!context) {
    ec = make_error_code(ibv_errc::ext_invalid_device);
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
    ec = make_error_code(ibv_errc::ext_no_available_device);
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

  if (devices.empty()) {
    ec = make_error_code(ibv_errc::ext_no_available_device);
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
