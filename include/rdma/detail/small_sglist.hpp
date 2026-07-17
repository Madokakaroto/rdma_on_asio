#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <memory>

namespace asio::rdma::detail {

template <class NativeSge>
struct built_sglist {
  NativeSge* data = nullptr;
  std::size_t count = 0;
  std::size_t total_bytes = 0;
  bool all_empty = true;
  bool too_many_sge = false;
  bool buffer_too_large = false;
  bool heap_spilled = false;
};

template <class NativeSge, std::size_t InlineCount = 8>
class small_sglist {
 public:
  static constexpr std::size_t inline_sge_count = InlineCount;

  small_sglist() = default;
  ~small_sglist() = default;
  small_sglist(small_sglist const&) = delete;
  small_sglist& operator=(small_sglist const&) = delete;

  void clear() noexcept {
    data_ = nullptr;
    size_ = 0;
  }

  // Allocation follows the normal C++ throwing-allocation contract. In
  // particular, std::bad_alloc is not translated to an error_code.
  void reserve(std::size_t count) {
    if (count <= inline_sge_count) {
      return;
    }
    if (count > heap_capacity_) {
      auto heap = std::make_unique<NativeSge[]>(count);
      if (data_) {
        std::copy_n(data_, size_, heap.get());
      }
      heap_ = std::move(heap);
      heap_capacity_ = count;
      if (size_ > inline_sge_count) {
        data_ = heap_.get();
      }
    }
  }

  void resize(std::size_t count) {
    if (count == 0) {
      clear();
      return;
    }

    auto const old_data = data_;
    auto const old_size = size_;
    auto const move_heap_to_inline =
        count <= inline_sge_count && old_data == heap_.get();
    reserve(count);
    auto* new_data = count <= inline_sge_count ? inline_.data() : heap_.get();
    if (move_heap_to_inline) {
      std::copy_n(old_data, (std::min)(old_size, count), new_data);
    }
    data_ = new_data;
    size_ = count;
  }

  NativeSge& append_uninitialized() {
    auto const index = size_;
    auto const target = size_ + 1;
    if (target > inline_sge_count && target > heap_capacity_) {
      auto const doubled = heap_capacity_ == 0 ? inline_sge_count * 2
                                               : heap_capacity_ * 2;
      reserve((std::max)(target, doubled));
    }
    if (!data_) {
      data_ = inline_.data();
    } else if (target > inline_sge_count) {
      data_ = heap_.get();
    }
    size_ = target;
    return data_[index];
  }

  NativeSge* data() noexcept { return data_; }
  NativeSge const* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept {
    return data_ == heap_.get() ? heap_capacity_ : inline_sge_count;
  }
  bool uses_heap() const noexcept { return data_ == heap_.get(); }
  NativeSge& operator[](std::size_t i) noexcept { return data_[i]; }
  NativeSge const& operator[](std::size_t i) const noexcept { return data_[i]; }

 private:
  std::array<NativeSge, inline_sge_count> inline_{};
  std::unique_ptr<NativeSge[]> heap_;
  std::size_t heap_capacity_ = 0;
  NativeSge* data_ = nullptr;
  std::size_t size_ = 0;
};

}
