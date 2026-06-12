#include <array>
#include <cstdint>
#include <list>
#include <type_traits>
#include <vector>

#include "unit_test.hpp"

#include "rdma/rdma_buffer.hpp"

namespace rdma = asio::rdma;

struct fake_memory_region
{
  std::array<unsigned char, 16> storage{};
  std::uint32_t key = 0x12345678;

  void* addr() noexcept { return storage.data(); }
  void const* addr() const noexcept { return storage.data(); }
  std::size_t length() const noexcept { return storage.size(); }
  std::uint32_t local_key() const noexcept { return key; }

  bool is_in_mr(std::size_t offset, std::size_t n) const noexcept
  {
    return offset <= storage.size() && n <= storage.size() - offset;
  }
};

void default_state()
{
  rdma::mutable_buffer mb;
  rdma::const_buffer cb;

  ASIO_CHECK(mb.addr() == nullptr);
  ASIO_CHECK(mb.data() == nullptr);
  ASIO_CHECK(mb.length() == 0);
  ASIO_CHECK(mb.local_key() == 0);

  ASIO_CHECK(cb.addr() == nullptr);
  ASIO_CHECK(cb.data() == nullptr);
  ASIO_CHECK(cb.length() == 0);
  ASIO_CHECK(cb.local_key() == 0);
}

void mutable_to_const_preserves_fields()
{
  int value = 0;
  rdma::mutable_buffer mb(&value, sizeof(value), 7);
  rdma::const_buffer cb = mb;

  ASIO_CHECK(cb.addr() == &value);
  ASIO_CHECK(cb.data() == &value);
  ASIO_CHECK(cb.length() == sizeof(value));
  ASIO_CHECK(cb.local_key() == 7);
}

void single_buffer_sequence_adl()
{
  int value = 0;
  rdma::mutable_buffer mb(&value, sizeof(value), 11);
  rdma::const_buffer cb(&value, sizeof(value), 12);

  auto mb_first = buffer_sequence_begin(mb);
  auto mb_last = buffer_sequence_end(mb);
  ASIO_CHECK(mb_first != mb_last);
  ASIO_CHECK(mb_first + 1 == mb_last);
  ASIO_CHECK(mb_first->addr() == &value);

  auto cb_first = buffer_sequence_begin(cb);
  auto cb_last = buffer_sequence_end(cb);
  ASIO_CHECK(cb_first != cb_last);
  ASIO_CHECK(cb_first + 1 == cb_last);
  ASIO_CHECK(cb_first->addr() == &value);
}

void standard_sequences_satisfy_concepts()
{
  static_assert(rdma::mr_adapted_buffer_sequence<rdma::mutable_buffer>);
  static_assert(rdma::mr_adapted_buffer_sequence<rdma::const_buffer>);
  static_assert(
      rdma::mr_adapted_buffer_sequence<std::vector<rdma::mutable_buffer>>);
  static_assert(
      rdma::mr_adapted_buffer_sequence<std::array<rdma::const_buffer, 2>>);
  static_assert(
      rdma::mr_adapted_buffer_sequence<std::list<rdma::mutable_buffer>>);
  static_assert(
      rdma::mr_const_buffer_sequence<std::vector<rdma::const_buffer>>);
  static_assert(
      rdma::mr_mutable_buffer_sequence<std::vector<rdma::mutable_buffer>>);
}

void buffer_size_and_all_empty()
{
  std::vector<rdma::mutable_buffer> empty;
  ASIO_CHECK(rdma::detail::buffer_size(empty) == 0);
  ASIO_CHECK(rdma::detail::all_empty(empty));

  int values[3] = {};
  std::array<rdma::mutable_buffer, 3> all_empty = {
      rdma::mutable_buffer(values, 0, 1),
      rdma::mutable_buffer(values + 1, 0, 1),
      rdma::mutable_buffer(values + 2, 0, 1),
  };
  ASIO_CHECK(rdma::detail::buffer_size(all_empty) == 0);
  ASIO_CHECK(rdma::detail::all_empty(all_empty));

  std::list<rdma::mutable_buffer> mixed = {
      rdma::mutable_buffer(values, 4, 1),
      rdma::mutable_buffer(values + 1, 0, 1),
      rdma::mutable_buffer(values + 2, 8, 1),
  };
  ASIO_CHECK(rdma::detail::buffer_size(mixed) == 12);
  ASIO_CHECK(!rdma::detail::all_empty(mixed));
}

void memory_region_buffer_factories()
{
  fake_memory_region mr;
  fake_memory_region const& cmr = mr;

  auto mb = rdma::buffer(mr);
  static_assert(std::is_same_v<decltype(mb), rdma::mutable_buffer>);
  ASIO_CHECK(mb.addr() == mr.storage.data());
  ASIO_CHECK(mb.length() == mr.storage.size());
  ASIO_CHECK(mb.local_key() == mr.key);

  auto cb = rdma::buffer(cmr);
  static_assert(std::is_same_v<decltype(cb), rdma::const_buffer>);
  ASIO_CHECK(cb.addr() == mr.storage.data());
  ASIO_CHECK(cb.length() == mr.storage.size());
  ASIO_CHECK(cb.local_key() == mr.key);
}

void memory_region_slices()
{
  fake_memory_region mr;
  fake_memory_region const& cmr = mr;

  auto mb = rdma::buffer(mr, 4, 6);
  ASIO_CHECK(mb.addr() == mr.storage.data() + 4);
  ASIO_CHECK(mb.length() == 6);
  ASIO_CHECK(mb.local_key() == mr.key);

  auto cb = rdma::buffer(cmr, 8, 4);
  ASIO_CHECK(cb.addr() == mr.storage.data() + 8);
  ASIO_CHECK(cb.length() == 4);
  ASIO_CHECK(cb.local_key() == mr.key);

  auto zero_at_end = rdma::buffer(mr, mr.length(), 0);
  ASIO_CHECK(zero_at_end.addr() == mr.storage.data() + mr.length());
  ASIO_CHECK(zero_at_end.length() == 0);
  ASIO_CHECK(zero_at_end.local_key() == mr.key);

  auto invalid = rdma::buffer(mr, 15, 2);
  ASIO_CHECK(invalid.addr() == nullptr);
  ASIO_CHECK(invalid.length() == 0);
  ASIO_CHECK(invalid.local_key() == 0);
}

ASIO_TEST_SUITE
(
  "rdma/buffer",
  ASIO_TEST_CASE(default_state)
  ASIO_TEST_CASE(mutable_to_const_preserves_fields)
  ASIO_TEST_CASE(single_buffer_sequence_adl)
  ASIO_COMPILE_TEST_CASE(standard_sequences_satisfy_concepts)
  ASIO_TEST_CASE(buffer_size_and_all_empty)
  ASIO_TEST_CASE(memory_region_buffer_factories)
  ASIO_TEST_CASE(memory_region_slices)
)
