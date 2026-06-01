#pragma once

#include <cstdint>
#include <iostream>

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
  class const_buffer {
   public:
    using rdma_buffer_tag = detail::rdma_const_buffer_tag;

   private:
    ibv_memory_region const& mr_;
    void const* addr_;
    std::size_t length_;

   public:
    explicit const_buffer(ibv_memory_region const& mr)
        : mr_(mr), addr_(nullptr), length_(0) {}
    const_buffer(ibv_memory_region const& mr, void const* addr, std::size_t length)
        : mr_(mr), addr_(addr), length_(length) {}
    const_buffer(const_buffer const&) = default;
    const_buffer& operator=(const_buffer const&) = delete;

    void const* addr() const noexcept { return addr_; }
    std::size_t length() const noexcept { return length_; }
    ibv_memory_region const& get_mr() const noexcept { return mr_; }
    std::uint32_t local_key() const { return get_mr().local_key(); }
    std::uint32_t remote_key() const { return get_mr().remote_key(); }

    friend const_buffer const* buffer_sequence_begin(
        const_buffer const& one) noexcept {
      return std::addressof(one);
    }
    friend const_buffer const* buffer_sequence_end(
        const_buffer const& one) noexcept {
      return std::addressof(one) + 1;
    }
  };

  class mutable_buffer {
   public:
    using rdma_buffer_tag = detail::rdma_mutable_buffer_tag;

   private:
    ibv_memory_region const& mr_;
    void* addr_;
    std::size_t length_;

   public:
    explicit mutable_buffer(ibv_memory_region const& mr)
        : mr_(mr), addr_(nullptr), length_(0) {}
    mutable_buffer(ibv_memory_region const& mr, void* addr, std::size_t length)
        : mr_(mr), addr_(addr), length_(length) {}
    mutable_buffer(mutable_buffer const&) = default;
    mutable_buffer& operator=(mutable_buffer const&) = delete;

    void* addr() const noexcept { return addr_; }
    std::size_t length() const noexcept { return length_; }
    ibv_memory_region const& get_mr() const noexcept { return mr_; }
    std::uint32_t local_key() const { return get_mr().local_key(); }
    std::uint32_t remote_key() const { return get_mr().remote_key(); }

    friend mutable_buffer const* buffer_sequence_begin(
        mutable_buffer const& one) noexcept {
      return std::addressof(one);
    }
    friend mutable_buffer const* buffer_sequence_end(
        mutable_buffer const& one) noexcept {
      return std::addressof(one) + 1;
    }
  };

 public:
  mutable_buffer slice(std::size_t offset, std::size_t length) {
    if (is_in_mr(offset, length)) {
      return mutable_buffer{
          *this, reinterpret_cast<std::uint8_t*>(addr()) + offset, length};
    }
    return mutable_buffer{*this};
  }

  const_buffer slice(std::size_t offset, std::size_t length) const {
    if (is_in_mr(offset, length)) {
      return const_buffer{
          *this, reinterpret_cast<std::uint8_t const*>(addr()) + offset,
          length};
    }
    return const_buffer{*this};
  }

  const_buffer cslice(std::size_t offset, std::size_t length) {
    return const_cast<ibv_memory_region const*>(this)->slice(offset, length);
  }

  const_buffer cslice(std::size_t offset, std::size_t length) const {
    return this->slice(offset, length);
  }
};

}
