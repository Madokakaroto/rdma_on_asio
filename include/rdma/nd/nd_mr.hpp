#pragma once

#include <cstddef>
#include <cstdint>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/rdma_buffer.hpp"
#include "rdma/nd/nd_device.hpp"
#include "rdma/nd/detail/nd_ops_verbs.hpp"

namespace asio::rdma {

// memory region raii type
class nd_memory_region {
 private:
  detail::nd2_memory_region_ptr mr_;
  void* addr_;
  std::size_t length_;
  mr_acccess_flag_t flag_;
  int extra_flag_;

 public:
  ASIO_DECL explicit nd_memory_region(
      nd_device_ptr const& device, void* addr, std::size_t length,
      mr_acccess_flag_t flag = mr_access_remote_write, int extra_flag = 0);

  ASIO_DECL ~nd_memory_region();

  nd_memory_region(nd_memory_region const&) = delete;
  nd_memory_region& operator=(nd_memory_region const&) = delete;
  nd_memory_region(nd_memory_region&&) = default;
  nd_memory_region& operator=(nd_memory_region&&) = default;

  std::uint32_t local_key() const {
    if (!mr_) {
      asio::detail::throw_error(rdma_errc::invalid_handle);
    }
    return mr_->GetLocalToken();
  }

  std::uint32_t remote_key() const {
    if (!mr_) {
      asio::detail::throw_error(rdma_errc::invalid_handle);
    }
    return mr_->GetRemoteToken();
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

  void const* addr() const noexcept {
    return addr_;
  }

  void* addr() noexcept {
    return addr_;
  }

  std::size_t length() const noexcept {
    return length_;
  }

  // Advertise a sub-range of this MR to the peer (the rkey/remote-access role,
  // distinct from the local-SGE buffer elements which only carry lkey/local
  // token). The returned {addr, remote token} is what the peer's RDMA
  // read/write targets.
  ASIO_DECL rdma_remote_addr_t remote_addr(std::size_t offset,
                                           std::size_t length) const;

 private:
  ASIO_DECL static detail::nd2_memory_region_ptr throw_reg_mr(
      nd_device_ptr const& device, void* addr, std::size_t length,
      mr_acccess_flag_t flag, int extra_flag);

 public:
  // slice/cslice return the shared, value-semantic asio::rdma::{mutable,const}_buffer.
  ASIO_DECL mutable_buffer slice(std::size_t offset, std::size_t length);

  ASIO_DECL const_buffer slice(std::size_t offset,
                               std::size_t length) const;

  ASIO_DECL mutable_buffer slice(void* addr, std::size_t length);

  ASIO_DECL const_buffer slice(void const* addr, std::size_t length) const;

  ASIO_DECL const_buffer cslice(std::size_t offset, std::size_t length);

  ASIO_DECL const_buffer cslice(std::size_t offset,
                                std::size_t length) const;

  ASIO_DECL const_buffer cslice(void const* addr, std::size_t length);

  ASIO_DECL const_buffer cslice(void const* addr,
                                std::size_t length) const;
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/impl/nd_mr.ipp"
#endif
