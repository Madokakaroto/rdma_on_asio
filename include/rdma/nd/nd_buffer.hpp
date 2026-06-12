#pragma once

#include "rdma/rdma_buffer.hpp"
#include "rdma/nd/nd_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ND2_SGE list.
namespace asio::rdma::detail {

template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, nd_sglist_t& sglist) {
  auto it = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  auto const size = std::distance(it, end);
  if (size > 0)
  {
    sglist.resize(size);
    // Forward-iterator traversal (++it), not begin + index: supports any forward
    // buffer sequence (std::list, asio-style iterators), not just random-access.
    for (std::size_t i = 0; it != end; ++it, ++i)
    {
      auto& sge = sglist[i];
      sge.Buffer = const_cast<void*>(it->addr());
      sge.BufferLength = it->length();
      sge.MemoryRegionToken = it->local_key();
    }
  }
}

}
