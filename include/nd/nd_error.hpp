#pragma once

#include <string>
#include <system_error>

#include "asio.hpp"
#include "ndstatus.h"

#ifndef NDEXT_NO_AVAILABLE_ADDRESS
#define NDEXT_NO_AVAILABLE_ADDRESS -1
#endif
#ifndef NDEXT_ALREADY_STOPT
#define NDEXT_ALREADY_STOPT -2
#endif
#ifndef NDEXT_INVALID_LISTENER
#define NDEXT_INVALID_LISTENER -3
#endif
#ifndef NDEXT_INVALID_CONNECTOR
#define NDEXT_INVALID_CONNECTOR -4
#endif
#ifndef NDEXT_INVALID_QP
#define NDEXT_INVALID_QP -5
#endif
#ifndef NDEXT_INVALID_CQ
#define NDEXT_INVALID_CQ -6
#endif
#ifndef NDEXT_INVALID_MR
#define NDEXT_INVALID_MR -7
#endif
#ifndef NDEXT_INVALID_DEVICE
#define NDEXT_INVALID_DEVICE -8
#endif
#ifndef NDEXT_ALREADY_REGISTERED
#define NDEXT_ALREADY_REGISTERED -9
#endif
#ifndef NDEXT_NO_EXECUTOR
#define NDEXT_NO_EXECUTOR -10
#endif
#ifndef NDEXT_NO_AVAILABLE_PROVIDER
#define NDEXT_NO_AVAILABLE_PROVIDER -11
#endif
#ifndef NDEXT_DEVICE_NOT_REGISTERED
#define NDEXT_DEVICE_NOT_REGISTERED -12
#endif
#ifndef NDEXT_DISCONNECTED
#define NDEXT_DISCONNECTED -13
#endif
#ifndef NDEXT_CONNECTOR_TERMINAL
#define NDEXT_CONNECTOR_TERMINAL -14
#endif



namespace asio::rdma {

enum class nd_errc : int {
  success = ND_SUCCESS,
  timeout = ND_TIMEOUT,
  pending = ND_PENDING,
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

  ext_no_available_address = NDEXT_NO_AVAILABLE_ADDRESS,
  ext_already_stopt = NDEXT_ALREADY_STOPT,
  ext_invalid_listener = NDEXT_INVALID_LISTENER,
  ext_invalid_connector = NDEXT_INVALID_CONNECTOR,
  ext_invalid_qp = NDEXT_INVALID_QP,
  ext_invalid_cq = NDEXT_INVALID_CQ,
  ext_invalid_mr = NDEXT_INVALID_MR,
  ext_invalid_device = NDEXT_INVALID_DEVICE,
  ext_already_registered = NDEXT_ALREADY_REGISTERED,
  ext_no_executor = NDEXT_NO_EXECUTOR,
  ext_no_available_provider = NDEXT_NO_AVAILABLE_PROVIDER,
  ext_device_not_registered = NDEXT_DEVICE_NOT_REGISTERED,
  // Connection torn down (NotifyDisconnect fired). Mirrors ibv_errc::ext_disconnected;
  // not mapped onto a socket error code (see disconnect_refactor_plan D-D).
  ext_disconnected = NDEXT_DISCONNECTED,
  // Connector is terminal (discarded): a prior disconnect()/NotifyDisconnect left
  // it torn down. async_connect early-exits with this instead of reusing the
  // stranded connector; the user must create a fresh one. Mirrors
  // ibv_errc::ext_connector_terminal.
  ext_connector_terminal = NDEXT_CONNECTOR_TERMINAL,
};

class nd_error_category : public std::error_category {
public:
  const char* name() const noexcept override {
    return "nd_error_code";
  }

  std::string message(int status) const override {
    switch(static_cast<HRESULT>(status)) {
      case ND_SUCCESS:
        return "ND_SUCCESS";
      case ND_TIMEOUT:
        return "ND_TIMEOUT";
      case ND_PENDING:
        return "ND_PENDING";
      case ND_BUFFER_OVERFLOW:
        return "ND_BUFFER_OVERFLOW";
      case ND_DEVICE_BUSY:
        return "ND_DEVICE_BUSY";
      case ND_NO_MORE_ENTRIES:
        return "ND_NO_MORE_ENTRIES";
      case ND_UNSUCCESSFUL:
        return "ND_UNSUCCESSFUL";
      case ND_ACCESS_VIOLATION:
        return "ND_ACCESS_VIOLATION";
      case ND_INVALID_HANDLE:
        return "ND_INVALID_HANDLE";
      case ND_INVALID_DEVICE_REQUEST:
        return "ND_INVALID_DEVICE_REQUEST";
      case ND_INVALID_PARAMETER:
        return "ND_INVALID_PARAMETER";
      case ND_NO_MEMORY:
        return "ND_NO_MEMORY";
      case ND_INVALID_PARAMETER_MIX:
        return "ND_INVALID_PARAMETER_MIX";
      case ND_DATA_OVERRUN:
        return "ND_DATA_OVERRUN";
      case ND_SHARING_VIOLATION:
        return "ND_SHARING_VIOLATION";
      case ND_INSUFFICIENT_RESOURCES:
        return "ND_INSUFFICIENT_RESOURCES";
      case ND_DEVICE_NOT_READY:
        return "ND_DEVICE_NOT_READY";
      case ND_IO_TIMEOUT:
        return "ND_IO_TIMEOUT";
      case ND_NOT_SUPPORTED:
        return "ND_NOT_SUPPORTED";
      case ND_INTERNAL_ERROR:
        return "ND_INTERNAL_ERROR";
      case ND_INVALID_PARAMETER_1:
        return "ND_INVALID_PARAMETER_1";
      case ND_INVALID_PARAMETER_2:
        return "ND_INVALID_PARAMETER_2";
      case ND_INVALID_PARAMETER_3:
        return "ND_INVALID_PARAMETER_3";
      case ND_INVALID_PARAMETER_4:
        return "ND_INVALID_PARAMETER_4";
      case ND_INVALID_PARAMETER_5:
        return "ND_INVALID_PARAMETER_5";
      case ND_INVALID_PARAMETER_6:
        return "ND_INVALID_PARAMETER_6";
      case ND_INVALID_PARAMETER_7:
        return "ND_INVALID_PARAMETER_7";
      case ND_INVALID_PARAMETER_8:
        return "ND_INVALID_PARAMETER_8";
      case ND_INVALID_PARAMETER_9:
        return "ND_INVALID_PARAMETER_9";
      case ND_INVALID_PARAMETER_10:
        return "ND_INVALID_PARAMETER_10";
      case ND_CANCELED:
        return "ND_CANCELED";
      case ND_REMOTE_ERROR:
        return "ND_REMOTE_ERROR";
      case ND_INVALID_ADDRESS:
        return "ND_INVALID_ADDRESS";
      case ND_INVALID_DEVICE_STATE:
        return "ND_INVALID_DEVICE_STATE";
      case ND_INVALID_BUFFER_SIZE:
        return "ND_INVALID_BUFFER_SIZE";
      case ND_TOO_MANY_ADDRESSES:
        return "ND_TOO_MANY_ADDRESSES";
      case ND_ADDRESS_ALREADY_EXISTS:
        return "ND_ADDRESS_ALREADY_EXISTS";
      case ND_CONNECTION_REFUSED:
        return "ND_CONNECTION_REFUSED";
      case ND_CONNECTION_INVALID:
        return "ND_CONNECTION_INVALID";
      case ND_CONNECTION_ACTIVE:
        return "ND_CONNECTION_ACTIVE";
      case ND_NETWORK_UNREACHABLE:
        return "ND_NETWORK_UNREACHABLE";
      case ND_HOST_UNREACHABLE:
        return "ND_HOST_UNREACHABLE";
      case ND_CONNECTION_ABORTED:
        return "ND_CONNECTION_ABORTED";
      case ND_DEVICE_REMOVED:
        return "ND_DEVICE_REMOVED";
      case NDEXT_NO_AVAILABLE_ADDRESS:
        return "ND_EXT no available address";
      case NDEXT_ALREADY_STOPT:
        return "ND_EXT already stopt";
      case NDEXT_INVALID_LISTENER:
        return "ND_EXT invalid listener";
      case NDEXT_INVALID_CONNECTOR:
        return "ND_EXT invalid connector";
      case NDEXT_INVALID_QP:
        return "ND_EXT invalid queue pair";
      case NDEXT_INVALID_CQ:
        return "ND_EXT invalid completion queue";
      case NDEXT_INVALID_MR:
        return "ND_EXT invalid memory region";
      case NDEXT_INVALID_DEVICE:
        return "ND_EXT invalid device";
      case NDEXT_ALREADY_REGISTERED:
        return "ND_EXT already registered";
      case NDEXT_NO_EXECUTOR:
        return "ND_EXT no executor";
      case NDEXT_NO_AVAILABLE_PROVIDER:
        return "ND_EXT no available provider";
      case NDEXT_DEVICE_NOT_REGISTERED:
        return "ND_EXT device not registered (call use_device first)";
      case NDEXT_DISCONNECTED:
        return "ND_EXT connection disconnected";
      case NDEXT_CONNECTOR_TERMINAL:
        return "ND_EXT connector is terminal (disconnected/failed); create a new connector";
      default:
        return "UNKNOWN_ND_ERROR";
    }
  }
};

inline std::error_category const& get_nd_error_category() {
  static nd_error_category instance{};
  return instance;
}

inline std::error_code make_nd_error_code(int e) {
  return std::error_code{ e, get_nd_error_category() };
}

inline std::error_code make_nd_error_code(HRESULT hr) {
  return std::error_code{ static_cast<int>(hr), get_nd_error_category() };
}

inline std::error_code make_error_code(nd_errc e) {
  return make_nd_error_code(static_cast<int>(e));
}

inline std::error_code make_system_error_code(int e) {
  return std::error_code{ e, std::system_category() };
}

inline void throw_error(std::error_code const& ec) {
  asio::detail::throw_error(ec);
}
}

namespace std {
template <>
struct is_error_code_enum<asio::rdma::nd_errc> : std::true_type {};
}