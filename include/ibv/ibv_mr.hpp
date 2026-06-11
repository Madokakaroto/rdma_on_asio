#pragma once

#include <cstdint>
#include <iostream>

#include "rdma/rdma_buffer.hpp"
#include "ibv/ibv_device.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

namespace asio::rdma {

// RAII deleter for ibv_mr
namespace detail {
struct ibv_mr_deleter {
  void operator()(native_mr_t* mr) const noexcept {
    if (mr) {
      ::ibv_dereg_mr(mr);
    }
  }
};
using unique_ibv_mr_ptr = std::unique_ptr<native_mr_t, ibv_mr_deleter>;
}

// memory region raii type (mirror nd_memory_region)
class ibv_memory_region {
 private:
  detail::unique_ibv_mr_ptr mr_;
  void* addr_;
  std::size_t length_;
  mr_acccess_flag_t flag_;
  int extra_flag_;

 public:
  explicit ibv_memory_region(ibv_device_ptr const& device, void* addr,
                    std::size_t length,
                    mr_acccess_flag_t flag = mr_access_remote_write,
                    int extra_flag = 0)
      : mr_(throw_reg_mr(device, addr, length, flag, extra_flag))
      , addr_(addr)
      , length_(length)
      , flag_(flag)
      , extra_flag_(extra_flag) {
  }

  ~ibv_memory_region() = default;

  ibv_memory_region(ibv_memory_region const&) = delete;
  ibv_memory_region& operator=(ibv_memory_region const&) = delete;
  ibv_memory_region(ibv_memory_region&&) = default;
  ibv_memory_region& operator=(ibv_memory_region&&) = default;

  std::uint32_t local_key() const {
    if (!mr_) {
      asio::detail::throw_error(make_error_code(ibv_errc::ext_invalid_device));
    }
    return mr_->lkey;
  }

  std::uint32_t remote_key() const {
    if (!mr_) {
      asio::detail::throw_error(make_error_code(ibv_errc::ext_invalid_device));
    }
    return mr_->rkey;
  }

  bool is_in_mr(void const* addr, std::size_t length) const noexcept {
    if (!mr_) {
      return false;
    }
    auto const diff = static_cast<char const*>(addr) -
                      static_cast<char const*>(addr_);
    if (diff < 0 || static_cast<std::size_t>(diff) >= length_) {
      return false;
    }
    return static_cast<std::size_t>(diff) + length <= length_;
  }

  bool is_in_mr(std::size_t offset, std::size_t length) const noexcept {
    return offset + length <= this->length();
  }

  void const* addr() const noexcept { return addr_; }
  void* addr() noexcept { return addr_; }
  std::size_t length() const noexcept { return length_; }

  // Advertise a sub-range of this MR to the peer (the rkey/remote-access role,
  // distinct from the local-SGE buffer elements which only carry lkey). The
  // returned {addr, rkey} is what the peer's RDMA read/write targets.
  rdma_remote_addr_t remote_addr(std::size_t offset, std::size_t length) const {
    if (!is_in_mr(offset, length)) {
      return rdma_remote_addr_t{0, 0};
    }
    return rdma_remote_addr_t{
        reinterpret_cast<std::uint64_t>(addr_) + offset, remote_key()};
  }

 private:
  static detail::unique_ibv_mr_ptr throw_reg_mr(ibv_device_ptr const& device,
                                                void* addr, std::size_t length,
                                                mr_acccess_flag_t flag,
                                                int extra_flag) {
    if (!device || !device->pd_) {
      asio::detail::throw_error(make_error_code(ibv_errc::ext_invalid_device));
    }
    asio::error_code ec{};
    detail::unique_ibv_mr_ptr result{detail::verbs_ops::reg_mr(
        device->pd_.get(), addr, length, flag, extra_flag, ec)};
    asio::detail::throw_error(ec);
    return result;
  }

 public:
  // slice/cslice return the shared, value-semantic asio::rdma::{mutable,const}_buffer.
  mutable_buffer slice(std::size_t offset, std::size_t length) {
    return asio::rdma::buffer(*this, offset, length);
  }

  const_buffer slice(std::size_t offset, std::size_t length) const {
    return asio::rdma::buffer(*this, offset, length);
  }

  const_buffer cslice(std::size_t offset, std::size_t length) {
    return const_cast<ibv_memory_region const*>(this)->slice(offset, length);
  }

  const_buffer cslice(std::size_t offset, std::size_t length) const {
    return this->slice(offset, length);
  }
};

}
