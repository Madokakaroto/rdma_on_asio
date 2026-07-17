#pragma once

#include <limits>

#include "rdma/rdma_buffer.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ibv_sge list.
namespace asio::rdma::detail {

inline void fill_native_sge(native_sge_t& sge, const_buffer const& buffer) {
  sge.addr = reinterpret_cast<std::uint64_t>(buffer.addr());
  sge.length = static_cast<std::uint32_t>(buffer.length());
  sge.lkey = buffer.local_key();
}

inline void fill_native_sge(native_sge_t& sge, mutable_buffer const& buffer) {
  sge.addr = reinterpret_cast<std::uint64_t>(buffer.addr());
  sge.length = static_cast<std::uint32_t>(buffer.length());
  sge.lkey = buffer.local_key();
}

template <mr_adapted_buffer_sequence BufferSequence>
inline built_sglist<native_sge_t> build_native_sglist(
    BufferSequence const& bs, ibv_sglist_t& sglist, std::uint32_t max_sge = 0) {
  built_sglist<native_sge_t> built;
  sglist.clear();

  auto it = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  constexpr auto max_segment =
      (std::numeric_limits<decltype(native_sge_t::length)>::max)();
  constexpr auto max_operation = (std::numeric_limits<std::uint32_t>::max)();
  for (; it != end; ++it) {
    if (max_sge != 0 && built.count >= max_sge) {
      built.too_many_sge = true;
      built.data = sglist.data();
      built.heap_spilled = sglist.uses_heap();
      return built;
    }
    auto const length = it->length();
    if (length > max_segment ||
        built.total_bytes > max_operation - length) {
      built.buffer_too_large = true;
      built.data = sglist.data();
      built.heap_spilled = sglist.uses_heap();
      return built;
    }
    auto& sge = sglist.append_uninitialized();
    fill_native_sge(sge, *it);
    ++built.count;
    built.total_bytes += length;
    built.all_empty = built.all_empty && length == 0;
  }

  built.data = sglist.data();
  built.heap_spilled = sglist.uses_heap();
  return built;
}

template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, ibv_sglist_t& sglist) {
  build_native_sglist(bs, sglist);
}

}
