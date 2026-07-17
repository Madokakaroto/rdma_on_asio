#pragma once

#include <string>
#include <system_error>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
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

  address_family_not_supported,

  // Shared library-level validation errors (ND and ibv).
  buffer_too_large,
  invalid_config,
};

// Non-template implementations live in rdma/impl/rdma_error.ipp:
// header-only mode includes it at the bottom (ASIO_DECL == inline); separate
// compilation compiles it once via rdma/impl/src.hpp.
class rdma_error_category : public std::error_category {
 public:
  ASIO_DECL const char* name() const noexcept override;
  ASIO_DECL std::string message(int status) const override;
};

ASIO_DECL std::error_category const& get_rdma_error_category();

ASIO_DECL asio::error_code make_error_code(rdma_errc e);

}  // namespace asio::rdma

namespace std {
template <>
struct is_error_code_enum<asio::rdma::rdma_errc> : true_type {};
}  // namespace std

#if defined(ASIO_HEADER_ONLY)
# include "rdma/impl/rdma_error.ipp"
#endif  // defined(ASIO_HEADER_ONLY)
