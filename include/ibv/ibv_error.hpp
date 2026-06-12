#pragma once

#include <cerrno>
#include <string>
#include <system_error>

#include "asio.hpp"
#include "rdma/rdma_error.hpp"

namespace asio::rdma {

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
