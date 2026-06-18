#pragma once

#include "rdma/rdma_buffer.hpp"
#include "rdma/nd/nd_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ND2_SGE list.
namespace asio::rdma::detail {

inline void fill_native_sge(native_sge_t& sge, const_buffer const& buffer) {
  sge.Buffer = const_cast<void*>(buffer.addr());
  sge.BufferLength = buffer.length();
  sge.MemoryRegionToken = buffer.local_key();
}

inline void fill_native_sge(native_sge_t& sge, mutable_buffer const& buffer) {
  sge.Buffer = buffer.addr();
  sge.BufferLength = buffer.length();
  sge.MemoryRegionToken = buffer.local_key();
}

template <mr_adapted_buffer_sequence BufferSequence>
inline built_sglist<native_sge_t> build_native_sglist(
    BufferSequence const& bs, nd_sglist_t& sglist, std::uint32_t max_sge = 0) {
  built_sglist<native_sge_t> built;
  sglist.clear();

  auto it = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  asio::error_code ec;
  for (; it != end; ++it) {
    if (max_sge != 0 && built.count >= max_sge) {
      built.too_many_sge = true;
      built.data = sglist.data();
      built.heap_spilled = sglist.uses_heap();
      return built;
    }
    auto& sge = sglist.append_uninitialized(ec);
    fill_native_sge(sge, *it);
    ++built.count;
    built.total_bytes += it->length();
    built.all_empty = built.all_empty && it->length() == 0;
  }

  built.data = sglist.data();
  built.heap_spilled = sglist.uses_heap();
  return built;
}

template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, nd_sglist_t& sglist) {
  build_native_sglist(bs, sglist);
}

}
