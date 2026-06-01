#pragma once

#include "rdma/rdma_buffer.hpp"
#include "nd/nd_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ND2_SGE list.
namespace asio::rdma::detail {

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

}
