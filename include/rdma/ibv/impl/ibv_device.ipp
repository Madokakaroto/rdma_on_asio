#ifndef RDMA_IBV_IMPL_IBV_DEVICE_IPP
#define RDMA_IBV_IMPL_IBV_DEVICE_IPP

#include "asio/detail/throw_error.hpp"

#include "rdma/ibv/ibv_device.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

asio::ip::address ibv_device_t::get_v4_address() const {
  if (!v4_address_) {
    asio::detail::throw_error(
        make_error_code(rdma_errc::address_family_not_supported));
  }
  return *v4_address_;
}

asio::ip::address ibv_device_t::get_v6_address() const {
  if (!v6_address_) {
    asio::detail::throw_error(
        make_error_code(rdma_errc::address_family_not_supported));
  }
  return *v6_address_;
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_DEVICE_IPP
