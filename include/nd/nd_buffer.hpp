#pragma once

#include "nd/nd_types.hpp"

namespace asio::rdma {
// buffer & buffer sequence concepts for memory region
/// general buffer from memory region
template <typename Buffer>
concept mr_buffer = requires { typename Buffer::nd_buffer_tag; };
template <typename BufferRef>
concept mr_buffer_ref = mr_buffer<std::remove_cvref_t<BufferRef>>;

/// const buffer from memory region
template <typename Buffer>
concept const_mr_buffer = requires {
  requires mr_buffer<Buffer>;
  requires std::same_as<typename Buffer::nd_buffer_tag,
                        detail::nd_const_buffer_tag>;
};
template <typename BufferRef>
concept const_mr_buffer_ref = const_mr_buffer<std::remove_cvref_t<BufferRef>>;

/// mutable buffer from memory region
template <typename Buffer>
concept mutable_mr_buffer = requires {
  requires mr_buffer<Buffer>;
  requires std::same_as<typename Buffer::nd_buffer_tag,
                        detail::nd_mutable_buffer_tag>;
};
template <typename BufferRef>
concept mutable_mr_buffer_ref =
    mutable_mr_buffer<std::remove_cvref_t<BufferRef>>;

/// buffer sequence
template <typename BufferSequence>
concept mr_buffer_sequence = requires(BufferSequence bs) {
  { *bs.cbegin() } -> mr_buffer_ref;
  { *bs.cend() } -> mr_buffer_ref;
  { std::distance(bs.cbegin(), bs.cend()) };
};

// buffer sequence begin & end
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

/// adapted buffer sequence
template <typename AdaptedBufferSequence>
concept mr_adapted_buffer_sequence = requires(AdaptedBufferSequence abs) {
  { *buffer_sequence_begin(abs) } -> mr_buffer_ref;
  { *buffer_sequence_end(abs) } -> mr_buffer_ref;
  { std::distance(buffer_sequence_end(abs), buffer_sequence_end(abs)) };
};

/// const buffer sequence
template <typename BufferSequence>
concept mr_const_buffer_sequence = requires(BufferSequence bs) {
  { *buffer_sequence_begin(bs) } -> const_mr_buffer_ref;
  { *buffer_sequence_end(bs) } -> const_mr_buffer_ref;
  { std::distance(buffer_sequence_end(bs), buffer_sequence_end(bs)) };
};

/// mutable buffer sequence
template <typename BufferSequence>
concept mr_mutable_buffer_sequence = requires(BufferSequence bs) {
  { *buffer_sequence_begin(bs) } -> mutable_mr_buffer_ref;
  { *buffer_sequence_end(bs) } -> mutable_mr_buffer_ref;
  { std::distance(buffer_sequence_end(bs), buffer_sequence_end(bs)) };
};

namespace detail {

/// buffer sequence to sge
template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, nd_sglist_t& sglist) {
  auto const begin = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  auto const size = std::distance(begin, end);
  if (size > 0)
  {
    sglist.resize(size);
    for (std::size_t loop = 0; loop < size; ++loop)
    {
      auto const buffer = begin + loop;
      auto& sge = sglist[loop];
      sge.Buffer = const_cast<void*>(buffer->addr());
      sge.BufferLength = buffer->length();
      sge.MemoryRegionToken = buffer->local_key();
    }
  }
}

template <mr_adapted_buffer_sequence BufferSequence>
inline std::size_t buffer_size(BufferSequence const& buffers) noexcept {
  return std::reduce(buffer_sequence_begin(buffers),
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