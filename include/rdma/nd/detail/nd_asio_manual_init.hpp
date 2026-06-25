#pragma once

#include <asio/detail/winsock_init.hpp>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4073)
#pragma init_seg(lib)
inline asio::detail::winsock_init<>::manual manual_winsock_init;
#pragma warning(pop)
#else  // using MinGw (gcc)
inline asio::detail::winsock_init<>::manual manual_winsock_init
    __attribute__((init_priority(101)));
#endif

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY

#include <ndsupport.h>
#include <iostream>
#include <system_error>

namespace asio::rdma::detail {
struct nd_global_t {
  int wsa_init_ { 0 };

  ASIO_DECL nd_global_t();

  ASIO_DECL ~nd_global_t();

  nd_global_t(nd_global_t const&) = delete;
  nd_global_t& operator=(nd_global_t const&) = delete;
  nd_global_t(nd_global_t&&) = delete;
  nd_global_t& operator=(nd_global_t&&) = delete;
};
}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_asio_manual_init.ipp"
#endif
