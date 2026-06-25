#ifndef RDMA_ND_DETAIL_IMPL_ND_ASIO_MANUAL_INIT_IPP
#define RDMA_ND_DETAIL_IMPL_ND_ASIO_MANUAL_INIT_IPP

#include <ndsupport.h>
#include <iostream>
#include <system_error>

#include "rdma/nd/detail/nd_asio_manual_init.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

nd_global_t::nd_global_t() {
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

nd_global_t::~nd_global_t() {
  auto const hr = ::NdCleanup();
  if (FAILED(hr)) {
    std::cerr << "Failed to call NdCleanup, exiting ...\n";
  }

  if (wsa_init_ != 0) {
    ::WSACleanup();
    wsa_init_ = 0;
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_ASIO_MANUAL_INIT_IPP
