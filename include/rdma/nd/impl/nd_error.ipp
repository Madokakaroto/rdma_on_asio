#ifndef RDMA_ND_IMPL_ND_ERROR_IPP
#define RDMA_ND_IMPL_ND_ERROR_IPP

#include "rdma/nd/nd_error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

const char* nd_error_category::name() const noexcept {
  return "nd_error_code";
}

std::string nd_error_category::message(int status) const {
  switch(static_cast<HRESULT>(status)) {
    case ND_SUCCESS:
      return "ND_SUCCESS";
    case ND_TIMEOUT:
      return "ND_TIMEOUT";
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
    default:
      return "UNKNOWN_ND_ERROR";
  }
}

std::error_category const& get_nd_error_category() {
  static nd_error_category instance{};
  return instance;
}

std::error_code make_nd_error_code(int e) {
  return std::error_code{ e, get_nd_error_category() };
}

std::error_code make_nd_error_code(HRESULT hr) {
  return std::error_code{ static_cast<int>(hr), get_nd_error_category() };
}

std::error_code make_error_code(nd_errc e) {
  return make_nd_error_code(static_cast<int>(e));
}

std::error_code make_system_error_code(int e) {
  return std::error_code{ e, std::system_category() };
}

void throw_error(std::error_code const& ec) {
  asio::detail::throw_error(ec);
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_ERROR_IPP
