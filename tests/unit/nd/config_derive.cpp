#include <WinSock2.h>
#include <ws2tcpip.h>

#include "unit_test.hpp"

#include "rdma/nd/detail/nd_config_derive.hpp"

namespace rdma = asio::rdma;

rdma::detail::native_context_config_t large_caps()
{
  rdma::detail::native_context_config_t c{};
  c.MaxCompletionQueueDepth = 8192;
  c.MaxInitiatorQueueDepth = 256;
  c.MaxReceiveQueueDepth = 512;
  c.MaxInitiatorSge = 8;
  c.MaxReceiveSge = 16;
  c.MaxInlineDataSize = 96;
  c.MaxInboundReadLimit = 4;
  c.MaxOutboundReadLimit = 7;
  return c;
}

void defaults_are_derived_from_caps_with_library_limits()
{
  auto effective =
      rdma::detail::derive_effective_config(rdma::nd_config_t{}, large_caps());

  ASIO_CHECK(effective.cqe_ == rdma::detail::default_cqe);
  ASIO_CHECK(effective.max_send_wr_ == rdma::detail::default_max_send_wr);
  ASIO_CHECK(effective.max_recv_wr_ == rdma::detail::default_max_recv_wr);
  ASIO_CHECK(effective.max_send_sge_ == rdma::detail::default_max_send_sge);
  ASIO_CHECK(effective.max_recv_sge_ == rdma::detail::default_max_recv_sge);
  ASIO_CHECK(effective.max_inline_data_ == 96);
  ASIO_CHECK(effective.inbound_read_limit_ == 4);
  ASIO_CHECK(effective.outbound_read_limit_ == 7);
}

void defaults_respect_small_device_caps()
{
  auto c = large_caps();
  c.MaxCompletionQueueDepth = 32;
  c.MaxInitiatorQueueDepth = 17;
  c.MaxReceiveQueueDepth = 19;
  c.MaxInitiatorSge = 2;
  c.MaxReceiveSge = 3;

  auto effective = rdma::detail::derive_effective_config(rdma::nd_config_t{}, c);

  ASIO_CHECK(effective.cqe_ == 32);
  ASIO_CHECK(effective.max_send_wr_ == 17);
  ASIO_CHECK(effective.max_recv_wr_ == 19);
  ASIO_CHECK(effective.max_send_sge_ == 2);
  ASIO_CHECK(effective.max_recv_sge_ == 3);
}

void explicit_user_values_are_preserved()
{
  rdma::nd_config_t requested{};
  requested.cqe_ = 64;
  requested.max_send_wr_ = 33;
  requested.max_recv_wr_ = 44;
  requested.max_send_sge_ = 2;
  requested.max_recv_sge_ = 3;
  requested.max_inline_data_ = 11;
  requested.inbound_read_limit_ = 1;
  requested.outbound_read_limit_ = 2;
  requested.backlog_ = 9;

  auto effective =
      rdma::detail::derive_effective_config(requested, large_caps());

  ASIO_CHECK(effective.cqe_ == requested.cqe_);
  ASIO_CHECK(effective.max_send_wr_ == requested.max_send_wr_);
  ASIO_CHECK(effective.max_recv_wr_ == requested.max_recv_wr_);
  ASIO_CHECK(effective.max_send_sge_ == requested.max_send_sge_);
  ASIO_CHECK(effective.max_recv_sge_ == requested.max_recv_sge_);
  ASIO_CHECK(effective.max_inline_data_ == requested.max_inline_data_);
  ASIO_CHECK(effective.inbound_read_limit_ == requested.inbound_read_limit_);
  ASIO_CHECK(effective.outbound_read_limit_ == requested.outbound_read_limit_);
  ASIO_CHECK(effective.backlog_ == requested.backlog_);
}

void zero_caps_stay_zero_for_derived_fields()
{
  rdma::detail::native_context_config_t c{};
  auto effective = rdma::detail::derive_effective_config(rdma::nd_config_t{}, c);

  ASIO_CHECK(effective.cqe_ == 0);
  ASIO_CHECK(effective.max_send_wr_ == 0);
  ASIO_CHECK(effective.max_recv_wr_ == 0);
  ASIO_CHECK(effective.max_send_sge_ == 0);
  ASIO_CHECK(effective.max_recv_sge_ == 0);
  ASIO_CHECK(effective.max_inline_data_ == 0);
  ASIO_CHECK(effective.inbound_read_limit_ == 0);
  ASIO_CHECK(effective.outbound_read_limit_ == 0);
}

void compatibility_checks_user_values_against_caps()
{
  auto c = large_caps();
  rdma::nd_config_t valid{};
  valid.cqe_ = 128;
  valid.max_send_wr_ = 128;
  valid.max_recv_wr_ = 128;
  valid.max_send_sge_ = 4;
  valid.max_recv_sge_ = 4;
  valid.inbound_read_limit_ = 2;
  valid.outbound_read_limit_ = 2;
  ASIO_CHECK(rdma::detail::is_config_compatible(valid, c));

  auto invalid = valid;
  invalid.max_recv_wr_ = 999;
  ASIO_CHECK(!rdma::detail::is_config_compatible(invalid, c));
}

ASIO_TEST_SUITE
(
  "nd/config_derive",
  ASIO_TEST_CASE(defaults_are_derived_from_caps_with_library_limits)
  ASIO_TEST_CASE(defaults_respect_small_device_caps)
  ASIO_TEST_CASE(explicit_user_values_are_preserved)
  ASIO_TEST_CASE(zero_caps_stay_zero_for_derived_fields)
  ASIO_TEST_CASE(compatibility_checks_user_values_against_caps)
)
