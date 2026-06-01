#pragma once

#include "rdma/rdma_buffer.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_impl_types.hpp"

// The buffer concepts + buffer_size/all_empty are shared (rdma/rdma_buffer.hpp).
// Only buffers2sglist is backend-specific: it fills the native ibv_sge list.
namespace asio::rdma::detail {

template <mr_adapted_buffer_sequence BufferSequence>
inline void buffers2sglist(BufferSequence const& bs, ibv_sglist_t& sglist) {
  auto const begin = buffer_sequence_begin(bs);
  auto const end = buffer_sequence_end(bs);
  auto const size = std::distance(begin, end);
  if (size > 0) {
    sglist.resize(static_cast<std::size_t>(size));
    for (std::size_t loop = 0; loop < static_cast<std::size_t>(size); ++loop) {
      auto const buffer = begin + loop;
      auto& sge = sglist[loop];
      sge.addr = reinterpret_cast<std::uint64_t>(buffer->addr());
      sge.length = static_cast<std::uint32_t>(buffer->length());
      sge.lkey = buffer->local_key();
    }
  }
}

}
