#ifndef RDMA_ND_IMPL_ND_MR_IPP
#define RDMA_ND_IMPL_ND_MR_IPP

#include <cstdint>
#include <iostream>

#include "rdma/nd/nd_mr.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

nd_memory_region::nd_memory_region(nd_device_ptr const& device, void* addr,
                                   std::size_t length,
                                   mr_acccess_flag_t flag, int extra_flag)
    : mr_(throw_reg_mr(device, addr, length, flag, extra_flag))
    , addr_(addr)
    , length_(length)
    , flag_(flag)
    , extra_flag_(extra_flag) {}

nd_memory_region::~nd_memory_region() {
  if (!mr_) {
    return;
  }

  asio::error_code ec{};
  detail::verbs_ops::dereg_mr(mr_.Get(), ec);
  if (ec) {
    std::cerr << "Failed to deregister memory region, errc(" << ec.value()
              << ") message(" << ec.message() << ")\n";
  }
}

rdma_remote_addr_t nd_memory_region::remote_addr(std::size_t offset,
                                                 std::size_t length) const {
  if (!is_in_mr(offset, length)) {
    return rdma_remote_addr_t{0, 0};
  }
  return rdma_remote_addr_t{
      reinterpret_cast<std::uint64_t>(addr_) + offset, remote_key()};
}

detail::nd2_memory_region_ptr nd_memory_region::throw_reg_mr(
    nd_device_ptr const& device, void* addr, std::size_t length,
    mr_acccess_flag_t flag, int extra_flag) {
  if (!device) {
    asio::detail::throw_error(rdma_errc::invalid_device);
  }
  asio::error_code ec{};
  detail::nd2_memory_region_ptr result{detail::verbs_ops::reg_mr(
      device->pd_.get(), addr, length, flag, extra_flag, ec)};
  asio::detail::throw_error(ec);
  return result;
}

mutable_buffer nd_memory_region::slice(std::size_t offset,
                                       std::size_t length) {
  return asio::rdma::buffer(*this, offset, length);
}

const_buffer nd_memory_region::slice(std::size_t offset,
                                     std::size_t length) const {
  return asio::rdma::buffer(*this, offset, length);
}

mutable_buffer nd_memory_region::slice(void* addr, std::size_t length) {
  auto const ptr_diff = reinterpret_cast<std::uint8_t*>(addr) -
                        reinterpret_cast<std::uint8_t*>(this->addr());
  if (ptr_diff > 0) {
    return slice(static_cast<std::size_t>(ptr_diff), length);
  }
  return mutable_buffer{};
}

const_buffer nd_memory_region::slice(void const* addr,
                                     std::size_t length) const {
  auto const ptr_diff = reinterpret_cast<std::uint8_t const*>(addr) -
                        reinterpret_cast<std::uint8_t const*>(this->addr());
  if (ptr_diff > 0) {
    return cslice(static_cast<std::size_t>(ptr_diff), length);
  }
  return const_buffer{};
}

const_buffer nd_memory_region::cslice(std::size_t offset,
                                      std::size_t length) {
  return const_cast<nd_memory_region const*>(this)->slice(offset, length);
}

const_buffer nd_memory_region::cslice(std::size_t offset,
                                      std::size_t length) const {
  return this->slice(offset, length);
}

const_buffer nd_memory_region::cslice(void const* addr,
                                      std::size_t length) {
  return const_cast<nd_memory_region const*>(this)->slice(addr, length);
}

const_buffer nd_memory_region::cslice(void const* addr,
                                      std::size_t length) const {
  return this->slice(addr, length);
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_MR_IPP
