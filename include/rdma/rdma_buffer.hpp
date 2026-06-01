#pragma once

#include <iterator>
#include <numeric>
#include <ranges>

#include "rdma/rdma_commons.hpp"

// Backend-independent memory-region buffer concepts and helpers shared by nd
// and ibv. The only platform-specific piece is buffers2sglist (it fills the
// native SGE type), which stays in each backend's buffer header.
namespace asio::rdma {

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
