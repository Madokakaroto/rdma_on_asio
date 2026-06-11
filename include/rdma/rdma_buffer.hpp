#pragma once

#include <concepts>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <ranges>
#include <type_traits>

#include "rdma/rdma_commons.hpp"

// Backend-independent memory-region buffer concepts, value-semantic buffer
// elements, the asio::buffer-style buffer() factory, and helpers -- all shared
// by nd and ibv. The only platform-specific piece is buffers2sglist (it fills
// the native SGE type), which stays in each backend's buffer header.
namespace asio::rdma {

// ---------------------------------------------------------------------------
// Value-semantic SGL buffer elements (cross-platform; NOT per-backend).
//
// One element = a {addr, length, lkey} view into a registered region. It is
// value-semantic (default-constructible + copyable/assignable) so it can live
// in std::vector / std::array / initializer_list -> real scatter/gather.
//
// Only lkey is carried: the SGE (struct ibv_sge / ND2_SGE), filled by
// buffers2sglist, is the sole consumer and needs only {addr, length, lkey}.
// rkey / advertising "my memory to the peer" is a different role, handled by
// the MR (memory_region::remote_addr(offset, length) -> rdma_remote_addr_t).
// ---------------------------------------------------------------------------
class mutable_buffer;

class const_buffer {
 public:
  using rdma_buffer_tag = detail::rdma_const_buffer_tag;

  const_buffer() = default;
  const_buffer(void const* addr, std::size_t length,
               std::uint32_t lkey) noexcept
      : addr_(addr), length_(length), lkey_(lkey) {}
  // Implicit mutable -> const (mirrors asio::const_buffer). Defined below.
  const_buffer(mutable_buffer const& b) noexcept;

  void const* addr() const noexcept { return addr_; }
  void const* data() const noexcept { return addr_; }
  std::size_t length() const noexcept { return length_; }
  std::uint32_t local_key() const noexcept { return lkey_; }

  friend const_buffer const* buffer_sequence_begin(
      const_buffer const& one) noexcept {
    return std::addressof(one);
  }
  friend const_buffer const* buffer_sequence_end(
      const_buffer const& one) noexcept {
    return std::addressof(one) + 1;
  }

 private:
  void const* addr_ = nullptr;
  std::size_t length_ = 0;
  std::uint32_t lkey_ = 0;
};

class mutable_buffer {
 public:
  using rdma_buffer_tag = detail::rdma_mutable_buffer_tag;

  mutable_buffer() = default;
  mutable_buffer(void* addr, std::size_t length, std::uint32_t lkey) noexcept
      : addr_(addr), length_(length), lkey_(lkey) {}

  void* addr() const noexcept { return addr_; }
  void* data() const noexcept { return addr_; }
  std::size_t length() const noexcept { return length_; }
  std::uint32_t local_key() const noexcept { return lkey_; }

  friend mutable_buffer const* buffer_sequence_begin(
      mutable_buffer const& one) noexcept {
    return std::addressof(one);
  }
  friend mutable_buffer const* buffer_sequence_end(
      mutable_buffer const& one) noexcept {
    return std::addressof(one) + 1;
  }

 private:
  void* addr_ = nullptr;
  std::size_t length_ = 0;
  std::uint32_t lkey_ = 0;
};

inline const_buffer::const_buffer(mutable_buffer const& b) noexcept
    : addr_(b.addr()), length_(b.length()), lkey_(b.local_key()) {}

// rdma_* aliases (naming-convention parity with rdma_memory_region etc.).
using rdma_const_buffer = const_buffer;
using rdma_mutable_buffer = mutable_buffer;

// ---------------------------------------------------------------------------
// buffer() factory -- asio::buffer-style, cross-platform. It only touches the
// MR's portable interface (addr/length/local_key/is_in_mr), so it is a single
// template over the MR type -- no per-backend overloads. mutable_buffer for a
// non-const MR, const_buffer for a const MR (mirrors asio::buffer's selection).
// ---------------------------------------------------------------------------
template <typename MR>
concept memory_region_like = requires(MR& mr, std::size_t off, std::size_t n) {
  { mr.length() } -> std::convertible_to<std::size_t>;
  { mr.local_key() } -> std::convertible_to<std::uint32_t>;
  { mr.is_in_mr(off, n) } -> std::convertible_to<bool>;
};

template <typename MR>
  requires memory_region_like<MR> && (!std::is_const_v<MR>)
inline mutable_buffer buffer(MR& mr) {
  return mutable_buffer{mr.addr(), mr.length(), mr.local_key()};
}

template <typename MR>
  requires memory_region_like<MR>
inline const_buffer buffer(MR const& mr) {
  return const_buffer{mr.addr(), mr.length(), mr.local_key()};
}

template <typename MR>
  requires memory_region_like<MR> && (!std::is_const_v<MR>)
inline mutable_buffer buffer(MR& mr, std::size_t offset, std::size_t n) {
  if (!mr.is_in_mr(offset, n)) return mutable_buffer{};
  return mutable_buffer{static_cast<std::uint8_t*>(mr.addr()) + offset, n,
                        mr.local_key()};
}

template <typename MR>
  requires memory_region_like<MR>
inline const_buffer buffer(MR const& mr, std::size_t offset, std::size_t n) {
  if (!mr.is_in_mr(offset, n)) return const_buffer{};
  return const_buffer{static_cast<std::uint8_t const*>(mr.addr()) + offset, n,
                      mr.local_key()};
}

template <typename Buffer>
concept mr_buffer = requires { typename Buffer::rdma_buffer_tag; };
template <typename BufferRef>
concept mr_buffer_ref = mr_buffer<std::remove_cvref_t<BufferRef>>;

template <typename Buffer>
concept const_mr_buffer = requires {
  requires mr_buffer<Buffer>;
  requires std::same_as<typename Buffer::rdma_buffer_tag,
                        detail::rdma_const_buffer_tag>;
};
template <typename BufferRef>
concept const_mr_buffer_ref = const_mr_buffer<std::remove_cvref_t<BufferRef>>;

template <typename Buffer>
concept mutable_mr_buffer = requires {
  requires mr_buffer<Buffer>;
  requires std::same_as<typename Buffer::rdma_buffer_tag,
                        detail::rdma_mutable_buffer_tag>;
};
template <typename BufferRef>
concept mutable_mr_buffer_ref =
    mutable_mr_buffer<std::remove_cvref_t<BufferRef>>;

template <typename BufferSequence>
concept mr_buffer_sequence = requires(BufferSequence bs) {
  { *bs.cbegin() } -> mr_buffer_ref;
  { *bs.cend() } -> mr_buffer_ref;
  { std::distance(bs.cbegin(), bs.cend()) };
};

template <mr_buffer_sequence BufferSequence>
inline decltype(auto) buffer_sequence_begin(BufferSequence const& bs) noexcept(
    noexcept(bs.cbegin())) {
  return bs.cbegin();
}

template <mr_buffer_sequence BufferSequence>
inline decltype(auto) buffer_sequence_end(BufferSequence const& bs) noexcept(
    noexcept(bs.cend())) {
  return bs.cend();
}

template <typename AdaptedBufferSequence>
concept mr_adapted_buffer_sequence = requires(AdaptedBufferSequence abs) {
  { *buffer_sequence_begin(abs) } -> mr_buffer_ref;
  { *buffer_sequence_end(abs) } -> mr_buffer_ref;
  { std::distance(buffer_sequence_begin(abs), buffer_sequence_end(abs)) };
};

template <typename BufferSequence>
concept mr_const_buffer_sequence = requires(BufferSequence bs) {
  { *buffer_sequence_begin(bs) } -> const_mr_buffer_ref;
  { *buffer_sequence_end(bs) } -> const_mr_buffer_ref;
  { std::distance(buffer_sequence_begin(bs), buffer_sequence_end(bs)) };
};

template <typename BufferSequence>
concept mr_mutable_buffer_sequence = requires(BufferSequence bs) {
  { *buffer_sequence_begin(bs) } -> mutable_mr_buffer_ref;
  { *buffer_sequence_end(bs) } -> mutable_mr_buffer_ref;
  { std::distance(buffer_sequence_begin(bs), buffer_sequence_end(bs)) };
};

namespace detail {

template <mr_adapted_buffer_sequence BufferSequence>
inline std::size_t buffer_size(BufferSequence const& buffers) noexcept {
  return std::accumulate(buffer_sequence_begin(buffers),
                         buffer_sequence_end(buffers), std::size_t{0},
                         [](std::size_t acc, auto const& buffer) {
                           return acc + buffer.length();
                         });
}

template <mr_adapted_buffer_sequence BufferSequence>
inline bool all_empty(BufferSequence const& buffers) noexcept {
  return std::ranges::all_of(buffer_sequence_begin(buffers),
                             buffer_sequence_end(buffers),
                             [](auto const& buffer) {
                               return buffer.length() == 0;
                             });
}

}
}
