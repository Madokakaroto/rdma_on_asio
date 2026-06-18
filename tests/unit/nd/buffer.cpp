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

void sglist_spills_to_heap_after_inline_capacity()
{
  std::array<rdma::mutable_buffer,
             rdma::detail::nd_sglist_t::inline_sge_count + 1>
      buffers{};
  std::array<unsigned char, buffers.size()> storage{};
  for (std::size_t i = 0; i != buffers.size(); ++i)
  {
    buffers[i] = rdma::mutable_buffer(storage.data() + i, 1,
                                      static_cast<std::uint32_t>(100 + i));
  }
  rdma::detail::nd_sglist_t sglist;

  auto built = rdma::detail::build_native_sglist(buffers, sglist);

  ASIO_CHECK(built.count == buffers.size());
  ASIO_CHECK(built.total_bytes == buffers.size());
  ASIO_CHECK(!built.all_empty);
  ASIO_CHECK(!built.too_many_sge);
  ASIO_CHECK(built.heap_spilled);
  ASIO_CHECK(sglist.uses_heap());
  ASIO_CHECK(sglist.size() == buffers.size());
  ASIO_CHECK(sglist[0].Buffer == buffers[0].addr());
  ASIO_CHECK(sglist[8].MemoryRegionToken == buffers[8].local_key());
}

void sglist_reuses_heap_capacity()
{
  rdma::detail::nd_sglist_t sglist;
  sglist.resize(rdma::detail::nd_sglist_t::inline_sge_count + 1);
  auto* heap = sglist.data();

  sglist.resize(rdma::detail::nd_sglist_t::inline_sge_count);
  ASIO_CHECK(!sglist.uses_heap());

  sglist.resize(rdma::detail::nd_sglist_t::inline_sge_count + 1);
  ASIO_CHECK(sglist.uses_heap());
  ASIO_CHECK(sglist.data() == heap);
}

void builder_reports_all_empty_and_too_many_sge()
{
  std::array<unsigned char, 4> storage{};
  std::array<rdma::mutable_buffer, 2> empty = {
      rdma::mutable_buffer(storage.data(), 0, 10),
      rdma::mutable_buffer(storage.data() + 1, 0, 11),
  };
  rdma::detail::nd_sglist_t empty_sglist;
  auto empty_built = rdma::detail::build_native_sglist(empty, empty_sglist);
  ASIO_CHECK(empty_built.count == empty.size());
  ASIO_CHECK(empty_built.total_bytes == 0);
  ASIO_CHECK(empty_built.all_empty);
  ASIO_CHECK(!empty_built.too_many_sge);

  std::array<rdma::mutable_buffer, 3> too_many = {
      rdma::mutable_buffer(storage.data(), 1, 20),
      rdma::mutable_buffer(storage.data() + 1, 1, 21),
      rdma::mutable_buffer(storage.data() + 2, 1, 22),
  };
  rdma::detail::nd_sglist_t limited_sglist;
  auto limited =
      rdma::detail::build_native_sglist(too_many, limited_sglist, 2);
  ASIO_CHECK(limited.too_many_sge);
  ASIO_CHECK(limited.count == 2);
  ASIO_CHECK(limited.total_bytes == 2);
}

ASIO_TEST_SUITE
(
  "nd/buffer",
  ASIO_TEST_CASE(empty_sequence_leaves_sglist_empty)
  ASIO_TEST_CASE(single_buffer_maps_exact_fields)
  ASIO_TEST_CASE(multi_buffer_sequence_preserves_order)
  ASIO_TEST_CASE(forward_iterator_sequence_is_supported)
  ASIO_TEST_CASE(sglist_spills_to_heap_after_inline_capacity)
  ASIO_TEST_CASE(sglist_reuses_heap_capacity)
  ASIO_TEST_CASE(builder_reports_all_empty_and_too_many_sge)
)
