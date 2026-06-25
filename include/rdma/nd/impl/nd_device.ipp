#ifndef RDMA_ND_IMPL_ND_DEVICE_IPP
#define RDMA_ND_IMPL_ND_DEVICE_IPP

#include "rdma/nd/nd_device.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

namespace detail {

asio::ip::address nd_adapter_t::get_v4_address() const {
  if (!v4_address_) {
    asio::detail::throw_error(
        make_error_code(rdma_errc::address_family_not_supported));
  }
  return *v4_address_;
}

asio::ip::address nd_adapter_t::get_v6_address() const {
  if (!v6_address_) {
    asio::detail::throw_error(
        make_error_code(rdma_errc::address_family_not_supported));
  }
  return *v6_address_;
}

}  // namespace detail

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_DEVICE_IPP
