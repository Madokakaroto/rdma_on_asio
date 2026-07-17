#ifndef RDMA_IMPL_RDMA_ERROR_IPP
#define RDMA_IMPL_RDMA_ERROR_IPP

#include "rdma/rdma_error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

const char* rdma_error_category::name() const noexcept {
  return "rdma_error_code";
}

std::string rdma_error_category::message(int status) const {
  switch (static_cast<rdma_errc>(status)) {
    case rdma_errc::no_available_device:
      return "RDMA no available device";
    case rdma_errc::invalid_device:
      return "RDMA invalid device";
    case rdma_errc::invalid_handle:
      return "RDMA invalid object handle";
    case rdma_errc::already_registered:
      return "RDMA device already registered on this execution context";
    case rdma_errc::device_not_registered:
      return "RDMA device not registered (call use_device first)";
    case rdma_errc::disconnected:
      return "RDMA connection disconnected";
    case rdma_errc::device_removed:
      return "RDMA local device removed";
    case rdma_errc::connector_terminal:
      return "RDMA connector is terminal; create a new connector";
    case rdma_errc::too_many_sge:
      return "RDMA scatter/gather list exceeds device max_sge";
    case rdma_errc::private_data_too_large:
      return "RDMA outgoing private_data exceeds the CM cap";
    case rdma_errc::address_family_not_supported:
      return "RDMA device has no local address of the requested family";
    case rdma_errc::buffer_too_large:
      return "RDMA buffer length exceeds the supported native limit";
    case rdma_errc::invalid_config:
      return "RDMA configuration exceeds device capabilities or is incompatible";
    default:
      return "UNKNOWN_RDMA_ERROR";
  }
}

std::error_category const& get_rdma_error_category() {
  static rdma_error_category instance{};
  return instance;
}

asio::error_code make_error_code(rdma_errc e) {
  return {static_cast<int>(e), get_rdma_error_category()};
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IMPL_RDMA_ERROR_IPP
