#pragma once

#include <system_error>

#include "asio/error_code.hpp"

namespace asio::rdma {

enum class rdma_errc : int {
  no_available_device = 1,

  invalid_device,
  invalid_handle,

  already_registered,
  device_not_registered,

  disconnected,
  device_removed,
  connector_terminal,

  too_many_sge,
  private_data_too_large,
};

class rdma_error_category : public std::error_category {
 public:
  const char* name() const noexcept override { return "rdma_error_code"; }

  std::string message(int status) const override {
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
      default:
        return "UNKNOWN_RDMA_ERROR";
    }
  }
};

inline std::error_category const& get_rdma_error_category() {
  static rdma_error_category instance{};
  return instance;
}

inline asio::error_code make_error_code(rdma_errc e) {
  return {static_cast<int>(e), get_rdma_error_category()};
}

}  // namespace asio::rdma

namespace std {
template <>
struct is_error_code_enum<asio::rdma::rdma_errc> : true_type {};
}  // namespace std
