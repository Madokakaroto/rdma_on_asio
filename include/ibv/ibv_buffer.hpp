#pragma once

#include "rdma/rdma_buffer.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_impl_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ibv_sge list.
namespace asio::rdma::detail {

template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, ibv_sglist_t& sglist) {
  auto it = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  auto const size = std::distance(it, end);
  if (size > 0) {
    sglist.resize(static_cast<std::size_t>(size));
    // Forward-iterator traversal (++it), not begin + index: supports any forward
    // buffer sequence (std::list, asio-style iterators), not just random-access.
    for (std::size_t i = 0; it != end; ++it, ++i) {
      auto& sge = sglist[i];
      sge.addr = reinterpret_cast<std::uint64_t>(it->addr());
      sge.length = static_cast<std::uint32_t>(it->length());
      sge.lkey = it->local_key();
    }
  }
}

}
