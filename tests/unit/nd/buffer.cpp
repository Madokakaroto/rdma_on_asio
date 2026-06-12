#include <array>
#include <cstdint>
#include <list>
#include <vector>

#include <WinSock2.h>
#include <ws2tcpip.h>

#include "unit_test.hpp"

#include "rdma/nd/nd_buffer.hpp"

namespace rdma = asio::rdma;

void empty_sequence_leaves_sglist_empty()
{
  std::vector<rdma::mutable_buffer> buffers;
  rdma::detail::nd_sglist_t sglist;

  rdma::detail::buffers2sglist(buffers, sglist);

  ASIO_CHECK(sglist.size() == 0);
  ASIO_CHECK(sglist.data() == nullptr);
}

void single_buffer_maps_exact_fields()
{
  std::array<unsigned char, 8> storage{};
  rdma::mutable_buffer buffer(storage.data(), storage.size(), 0x44);
  rdma::detail::nd_sglist_t sglist;

  rdma::detail::buffers2sglist(buffer, sglist);

  ASIO_CHECK(sglist.size() == 1);
  ASIO_CHECK(sglist[0].Buffer == storage.data());
  ASIO_CHECK(sglist[0].BufferLength == storage.size());
  ASIO_CHECK(sglist[0].MemoryRegionToken == 0x44);
}

void multi_buffer_sequence_preserves_order()
{
  std::array<unsigned char, 12> storage{};
  std::vector<rdma::mutable_buffer> buffers = {
      rdma::mutable_buffer(storage.data(), 1, 10),
      rdma::mutable_buffer(storage.data() + 2, 3, 11),
      rdma::mutable_buffer(storage.data() + 6, 5, 12),
  };
  rdma::detail::nd_sglist_t sglist;

  rdma::detail::buffers2sglist(buffers, sglist);

  ASIO_CHECK(sglist.size() == buffers.size());
  for (std::size_t i = 0; i != buffers.size(); ++i)
  {
    ASIO_CHECK(sglist[i].Buffer == buffers[i].addr());
    ASIO_CHECK(sglist[i].BufferLength == buffers[i].length());
    ASIO_CHECK(sglist[i].MemoryRegionToken == buffers[i].local_key());
  }
}

void forward_iterator_sequence_is_supported()
{
  std::array<unsigned char, 4> a{};
  std::array<unsigned char, 4> b{};
  std::list<rdma::const_buffer> buffers = {
      rdma::const_buffer(a.data(), a.size(), 20),
      rdma::const_buffer(b.data(), b.size(), 21),
  };
  rdma::detail::nd_sglist_t sglist;

  rdma::detail::buffers2sglist(buffers, sglist);

  auto it = buffers.begin();
  ASIO_CHECK(sglist.size() == 2);
  ASIO_CHECK(sglist[0].Buffer == it->addr());
  ASIO_CHECK(sglist[0].BufferLength == it->length());
  ASIO_CHECK(sglist[0].MemoryRegionToken == it->local_key());
  ++it;
  ASIO_CHECK(sglist[1].Buffer == it->addr());
  ASIO_CHECK(sglist[1].BufferLength == it->length());
  ASIO_CHECK(sglist[1].MemoryRegionToken == it->local_key());
}

ASIO_TEST_SUITE
(
  "nd/buffer",
  ASIO_TEST_CASE(empty_sequence_leaves_sglist_empty)
  ASIO_TEST_CASE(single_buffer_maps_exact_fields)
  ASIO_TEST_CASE(multi_buffer_sequence_preserves_order)
  ASIO_TEST_CASE(forward_iterator_sequence_is_supported)
)
