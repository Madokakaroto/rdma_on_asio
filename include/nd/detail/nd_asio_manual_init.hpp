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

#include <ndsupport.h>
#include <iostream>
#include <system_error>

namespace asio::rdma::detail {
struct nd_global_t {
  int wsa_init_ { 0 };

  nd_global_t() {
    WSADATA wsaData;
    auto const ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0) {
      throw std::system_error(ret, std::system_category(), "WSAStartup failed");
    }
    wsa_init_ = 1;
    auto const hr = ::NdStartup();
    if (FAILED(hr)) {
      ::WSACleanup();
      wsa_init_ = 0;
      throw std::system_error(hr, std::system_category(), "NdStartup failed");
    }
  }

  ~nd_global_t() {
    auto const hr = ::NdCleanup();
    if (FAILED(hr)) {
      std::cerr << "Failed to call NdCleanup, exiting ...\n";
    }

    if (wsa_init_ != 0) {
      ::WSACleanup();
      wsa_init_ = 0;
    }
  }

  nd_global_t(nd_global_t const&) = delete;
  nd_global_t& operator=(nd_global_t const&) = delete;
  nd_global_t(nd_global_t&&) = delete;
  nd_global_t& operator=(nd_global_t&&) = delete;
};
}