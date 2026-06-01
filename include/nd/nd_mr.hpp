#pragma once

#include <iostream>
#include "nd/nd_device.hpp"
#include "nd/detail/nd_ops_verbs.hpp"

namespace asio::rdma {

// memory region raii type
class nd_mr_t {
 private:
  detail::nd2_memory_region_ptr mr_;
  void* addr_;
  std::size_t length_;
  mr_acccess_flag_t flag_;
  int extra_flag_;

 public:
  explicit nd_mr_t(nd_device_ptr const& device, void* addr, std::size_t length,
                   mr_acccess_flag_t flag = mr_access_remote_write,
                   int extra_flag = 0)
      : mr_(throw_reg_mr(device, addr, length, flag, extra_flag))
      , addr_(addr)
      , length_(length)
      , flag_(flag)
      , extra_flag_(extra_flag) {}

  ~nd_mr_t() {
    asio::error_code ec{};
    detail::verbs_ops::dereg_mr(mr_.Get(), ec);
    if (ec) {
      std::cerr << "Failed to deregister memory region, errc(" << ec.value()
                << ") message(" << ec.message() << ")\n";
    }
  }

  nd_mr_t(nd_mr_t const&) = delete;
  nd_mr_t& operator=(nd_mr_t const&) = delete;
  nd_mr_t(nd_mr_t&&) = default;
  nd_mr_t& operator=(nd_mr_t&&) = default;

  std::uint32_t local_key() const {
    if (!mr_) {
      asio::detail::throw_error(nd_errc::ext_invalid_mr);
    }
    return mr_->GetLocalToken();
  }

  std::uint32_t remote_key() const {
    if (!mr_) {
      asio::detail::throw_error(nd_errc::ext_invalid_mr);
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

 private:
  static detail::nd2_memory_region_ptr throw_reg_mr(nd_device_ptr const& device,
                                                    void* addr,
                                                    std::size_t length,
                                                    mr_acccess_flag_t flag,
                                                    int extra_flag) {
    
    if (!device) {
      asio::detail::throw_error(nd_errc::ext_invalid_device);
    }
    asio::error_code ec{};
    detail::nd2_memory_region_ptr result{detail::verbs_ops::reg_mr(
        device->pd_.get(), addr, length, flag, extra_flag, ec)};
    asio::detail::throw_error(ec);
    return result;
  }

 public:
  class const_buffer {
   public:
     using rdma_buffer_tag = detail::rdma_const_buffer_tag;

   private:
    nd_mr_t const& mr_;
    void const* addr_;
    std::size_t length_;

   public:
    explicit const_buffer(nd_mr_t const& mr)
        : mr_(mr), addr_(nullptr), length_(0) {}
    const_buffer(nd_mr_t const& mr, void const* addr, std::size_t length)
        : mr_(mr), addr_(addr), length_(length) {}
    const_buffer(const_buffer const&) = default;
    const_buffer& operator=(const_buffer const&) = delete;

   public:
    void const* addr() const noexcept {
      return addr_;
    }

    std::size_t length() const noexcept {
      return length_;
    }

    bool is_valid() const noexcept {
      return addr_ != nullptr && length_ >= 0 && mr_.is_in_mr(addr_, length_);
    }

    nd_mr_t const& get_mr() const noexcept {
      return mr_;
    }

    std::uint32_t local_key() const {
      return get_mr().local_key();
    }

    std::uint32_t remote_key() const {
      return get_mr().remote_key();
    }

    friend const_buffer const* buffer_sequence_begin(
        const_buffer const& one_buffer) noexcept {
      return std::addressof(one_buffer);
    }

    friend const_buffer const* buffer_sequence_end(
        const_buffer const& one_buffer) noexcept {
      return std::addressof(one_buffer) + 1;
    }
  };

  class mutable_buffer {
   public:
    using rdma_buffer_tag = detail::rdma_mutable_buffer_tag;

   private:
    nd_mr_t const& mr_;
    void* addr_;
    std::size_t length_;

   public:
    explicit mutable_buffer(nd_mr_t const& mr)
        : mr_(mr), addr_(nullptr), length_(0) {}
    mutable_buffer(nd_mr_t const& mr, void* addr, std::size_t length)
        : mr_(mr), addr_(addr), length_(length) {}
    mutable_buffer(mutable_buffer const&) = default;
    mutable_buffer& operator=(mutable_buffer const&) = delete;

   public:
    void* addr() const noexcept { 
      return addr_;
    }

    std::size_t length() const noexcept {
      return length_;
    }

    bool is_valid() const noexcept {
      return addr_ != nullptr && length_ >= 0 && mr_.is_in_mr(addr_, length_);
    }

    nd_mr_t const& get_mr() const noexcept { 
      return mr_;
    }

    std::uint32_t local_key() const {
      return get_mr().local_key();
    }

    std::uint32_t remote_key() const {
      return get_mr().remote_key();
    }

    friend mutable_buffer const* buffer_sequence_begin(
        mutable_buffer const& one_buffer) noexcept {
      return std::addressof(one_buffer);
    }

    friend mutable_buffer const* buffer_sequence_end(
        mutable_buffer const& one_buffer) noexcept {
      return std::addressof(one_buffer) + 1;
    }
  };

public:
  mutable_buffer slice(std::size_t offset, std::size_t length) {
    if (is_in_mr(offset, length)) {
      return mutable_buffer{
          *this, reinterpret_cast<uint8_t*>(this->addr()) + offset, length};
    }
    return mutable_buffer{*this};
  }

  const_buffer slice(std::size_t offset, std::size_t length) const {
    if (is_in_mr(offset, length)) {
      return const_buffer{
          *this, reinterpret_cast<uint8_t const*>(this->addr()) + offset,
          length};
    }
    return const_buffer{*this};
  }

  mutable_buffer slice(void* addr, std::size_t length) {
    auto const ptr_diff = reinterpret_cast<uint8_t*>(addr) -
                          reinterpret_cast<uint8_t*>(this->addr());
    if (ptr_diff > 0) {
      return slice(static_cast<std::size_t>(ptr_diff), length);
    }
    return mutable_buffer{*this};
  }

  const_buffer slice(void const* addr, std::size_t length) const {
    auto const ptr_diff = reinterpret_cast<uint8_t const*>(addr) -
                          reinterpret_cast<uint8_t const*>(this->addr());
    if (ptr_diff > 0) {
      return cslice(static_cast<size_t>(ptr_diff), length);
    }
    return const_buffer{*this};
  }

  const_buffer cslice(std::size_t offset, std::size_t length) {
    return const_cast<nd_mr_t const*>(this)->slice(offset, length);
  }

  const_buffer cslice(std::size_t offset, std::size_t length) const {
    return this->slice(offset, length);
  }

  const_buffer cslice(void const* addr, size_t length) {
    return const_cast<nd_mr_t const*>(this)->slice(addr, length);
  }

  const_buffer cslice(void const* addr, size_t length) const {
    return this->slice(addr, length);
  }
};

}