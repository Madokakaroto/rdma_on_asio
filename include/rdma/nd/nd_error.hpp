#pragma once

#include <string>
#include <system_error>

#include "asio.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "ndstatus.h"
#include "rdma/rdma_error.hpp"

namespace asio::rdma {

enum class nd_errc : int {
  success = ND_SUCCESS,
  timeout = ND_TIMEOUT,
  buffer_overflow = ND_BUFFER_OVERFLOW,
  device_busy = ND_DEVICE_BUSY,
  no_more_entries = ND_NO_MORE_ENTRIES,
  unsuccessful = ND_UNSUCCESSFUL,
  access_violation = ND_ACCESS_VIOLATION,
  invalid_handle = ND_INVALID_HANDLE,
  invalid_device_request = ND_INVALID_DEVICE_REQUEST,
  invalid_parameter = ND_INVALID_PARAMETER,
  no_memory = ND_NO_MEMORY,
  invalid_parameter_mix = ND_INVALID_PARAMETER_MIX,
  data_overrun = ND_DATA_OVERRUN,
  sharing_violation = ND_SHARING_VIOLATION,
  insufficient_resources = ND_INSUFFICIENT_RESOURCES,
  device_not_ready = ND_DEVICE_NOT_READY,
  io_timeout = ND_IO_TIMEOUT,
  not_supported = ND_NOT_SUPPORTED,
  internal_error = ND_INTERNAL_ERROR,
  invalid_parameter_1 = ND_INVALID_PARAMETER_1,
  invalid_parameter_2 = ND_INVALID_PARAMETER_2,
  invalid_parameter_3 = ND_INVALID_PARAMETER_3,
  invalid_parameter_4 = ND_INVALID_PARAMETER_4,
  invalid_parameter_5 = ND_INVALID_PARAMETER_5,
  invalid_parameter_6 = ND_INVALID_PARAMETER_6,
  invalid_parameter_7 = ND_INVALID_PARAMETER_7,
  invalid_parameter_8 = ND_INVALID_PARAMETER_8,
  invalid_parameter_9 = ND_INVALID_PARAMETER_9,
  invalid_parameter_10 = ND_INVALID_PARAMETER_10,
  canceled = ND_CANCELED,
  remote_error = ND_REMOTE_ERROR,
  invalid_address = ND_INVALID_ADDRESS,
  invalid_device_state = ND_INVALID_DEVICE_STATE,
  invalid_buffer_size = ND_INVALID_BUFFER_SIZE,
  too_many_addresses = ND_TOO_MANY_ADDRESSES,
  address_already_exists = ND_ADDRESS_ALREADY_EXISTS,
  connection_refused = ND_CONNECTION_REFUSED,
  connection_invalid = ND_CONNECTION_INVALID,
  connection_active = ND_CONNECTION_ACTIVE,
  network_unreachable = ND_NETWORK_UNREACHABLE,
  host_unreachable = ND_HOST_UNREACHABLE,
  connection_aborted = ND_CONNECTION_ABORTED,
  device_removed = ND_DEVICE_REMOVED,
};

class nd_error_category : public std::error_category {
public:
  ASIO_DECL const char* name() const noexcept override;

  ASIO_DECL std::string message(int status) const override;
};

ASIO_DECL std::error_category const& get_nd_error_category();

ASIO_DECL std::error_code make_nd_error_code(int e);

ASIO_DECL std::error_code make_nd_error_code(HRESULT hr);

ASIO_DECL std::error_code make_error_code(nd_errc e);

ASIO_DECL std::error_code make_system_error_code(int e);

ASIO_DECL void throw_error(std::error_code const& ec);

namespace detail {

inline asio::error_code completion_status_to_error(HRESULT status) {
  if (status == ND_SUCCESS) {
    return {};
  }
  if (status == ND_CANCELED) {
    return asio::error::operation_aborted;
  }
  return make_nd_error_code(status);
}

}  // namespace detail
}

namespace std {
template <>
struct is_error_code_enum<asio::rdma::nd_errc> : std::true_type {};
}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/impl/nd_error.ipp"
#endif
