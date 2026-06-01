#pragma once

#include <cerrno>
#include <string>
#include <system_error>

#include "asio.hpp"

// Library-level ("ext") error codes. Verbs/rdma_cm calls themselves report via
// errno (mapped through std::system_category); these cover failures that have no
// errno, mirroring the NDEXT_* codes on the nd backend.
#ifndef IBVEXT_NO_AVAILABLE_DEVICE
#define IBVEXT_NO_AVAILABLE_DEVICE -1
#endif
#ifndef IBVEXT_INVALID_DEVICE
#define IBVEXT_INVALID_DEVICE -2
#endif
#ifndef IBVEXT_ALREADY_REGISTERED
#define IBVEXT_ALREADY_REGISTERED -3
#endif

namespace asio::rdma {

enum class ibv_errc : int {
  ext_no_available_device = IBVEXT_NO_AVAILABLE_DEVICE,
  ext_invalid_device      = IBVEXT_INVALID_DEVICE,
  ext_already_registered  = IBVEXT_ALREADY_REGISTERED,
};

class ibv_error_category : public std::error_category {
 public:
  const char* name() const noexcept override { return "ibv_error_code"; }

  std::string message(int status) const override {
    switch (status) {
      case IBVEXT_NO_AVAILABLE_DEVICE:
        return "IBV_EXT no available device";
      case IBVEXT_INVALID_DEVICE:
        return "IBV_EXT invalid device";
      case IBVEXT_ALREADY_REGISTERED:
        return "IBV_EXT already registered";
      default:
        return "UNKNOWN_IBV_ERROR";
    }
  }
};

inline std::error_category const& get_ibv_error_category() {
  static ibv_error_category instance{};
  return instance;
}

inline std::error_code make_ibv_error_code(int e) {
  return std::error_code{ e, get_ibv_error_category() };
}

inline std::error_code make_error_code(ibv_errc e) {
  return make_ibv_error_code(static_cast<int>(e));
}

// verbs / rdma_cm calls report failure via errno
inline std::error_code make_system_error_code(int e) {
  return std::error_code{ e, std::system_category() };
}

// Capture the current errno. Call immediately after a failing libc/verbs call,
// before any other call can clobber errno.
inline std::error_code last_system_error() {
  return make_system_error_code(errno);
}

inline void throw_error(std::error_code const& ec) {
  asio::detail::throw_error(ec);
}

}

namespace std {
template <>
struct is_error_code_enum<asio::rdma::ibv_errc> : std::true_type {};
}
