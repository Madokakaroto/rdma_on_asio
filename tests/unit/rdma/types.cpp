#include <cstdint>
#include <type_traits>

#include "unit_test.hpp"

#include "rdma/rdma_commons.hpp"

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

ASIO_TEST_SUITE
(
  "rdma/types",
  ASIO_TEST_CASE(config_defaults_are_auto_derive_or_policy_values)
  ASIO_TEST_CASE(remote_addr_is_value_semantic)
  ASIO_TEST_CASE(completion_mode_has_expected_states)
)
