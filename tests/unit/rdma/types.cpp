#include <cstdint>
#include <limits>
#include <type_traits>

#include "unit_test.hpp"

#include "rdma/rdma_commons.hpp"
#include "rdma/detail/mr_range.hpp"

namespace rdma = asio::rdma;

void config_defaults_are_auto_derive_or_policy_values()
{
  rdma::rdma_config_t config{};

  ASIO_CHECK(config.cqe_ == 0);
  ASIO_CHECK(config.max_send_wr_ == 0);
  ASIO_CHECK(config.max_recv_wr_ == 0);
  ASIO_CHECK(config.max_send_sge_ == 0);
  ASIO_CHECK(config.max_recv_sge_ == 0);
  ASIO_CHECK(config.max_inline_data_ == 0);
  ASIO_CHECK(config.inbound_read_limit_ == 0);
  ASIO_CHECK(config.outbound_read_limit_ == 0);
  ASIO_CHECK(config.cm_resolve_timeout_ms_ == 0);
  ASIO_CHECK(config.backlog_ == 128);
}

void remote_addr_is_value_semantic()
{
  rdma::rdma_remote_addr_t addr{0x1234567887654321ULL, 0xAABBCCDD};
  auto copy = addr;

  ASIO_CHECK(copy.addr_ == addr.addr_);
  ASIO_CHECK(copy.token_ == addr.token_);

  copy.addr_ += 4;
  copy.token_ += 1;
  ASIO_CHECK(addr.addr_ == 0x1234567887654321ULL);
  ASIO_CHECK(addr.token_ == 0xAABBCCDD);
}

void completion_mode_has_expected_states()
{
  static_assert(std::is_enum_v<rdma::completion_mode>);

  ASIO_CHECK(rdma::completion_mode::none != rdma::completion_mode::event);
  ASIO_CHECK(rdma::completion_mode::none != rdma::completion_mode::poll);
  ASIO_CHECK(rdma::completion_mode::event != rdma::completion_mode::poll);
}

void mr_range_checks_are_overflow_safe()
{
  std::uint8_t storage[16]{};

  ASIO_CHECK(rdma::detail::mr_contains_offset(16, 0, 16));
  ASIO_CHECK(rdma::detail::mr_contains_offset(16, 16, 0));
  ASIO_CHECK(!rdma::detail::mr_contains_offset(
      16, (std::numeric_limits<std::size_t>::max)(), 1));
  ASIO_CHECK(!rdma::detail::mr_contains_offset(16, 15, 2));

  auto at_base = rdma::detail::mr_offset_of(storage, sizeof(storage),
                                             storage, sizeof(storage));
  auto at_end = rdma::detail::mr_offset_of(storage, sizeof(storage),
                                            storage + sizeof(storage), 0);
  ASIO_CHECK(at_base && *at_base == 0);
  ASIO_CHECK(at_end && *at_end == sizeof(storage));
  auto zero_region = rdma::detail::mr_offset_of(storage, 0, storage, 0);
  ASIO_CHECK(zero_region && *zero_region == 0);
  ASIO_CHECK(!rdma::detail::mr_offset_of(storage, sizeof(storage),
                                          storage + sizeof(storage), 1));

  auto const base = reinterpret_cast<std::uintptr_t>(storage);
  if (base != 0)
  {
    auto* before = reinterpret_cast<void const*>(base - 1);
    ASIO_CHECK(!rdma::detail::mr_offset_of(storage, sizeof(storage), before, 0));
  }
}

ASIO_TEST_SUITE
(
  "rdma/types",
  ASIO_TEST_CASE(config_defaults_are_auto_derive_or_policy_values)
  ASIO_TEST_CASE(remote_addr_is_value_semantic)
  ASIO_TEST_CASE(completion_mode_has_expected_states)
  ASIO_TEST_CASE(mr_range_checks_are_overflow_safe)
)
