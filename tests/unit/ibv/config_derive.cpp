#include "unit_test.hpp"

#include "rdma/ibv/detail/ibv_config_derive.hpp"

namespace rdma = asio::rdma;

rdma::detail::native_device_attr_t large_caps()
{
  rdma::detail::native_device_attr_t c{};
  c.max_cqe = 8192;
  c.max_qp_wr = 256;
  c.max_sge = 8;
  c.max_qp_rd_atom = 4;
  c.max_qp_init_rd_atom = 7;
  return c;
}

void cap_of_clamps_non_positive_values()
{
  ASIO_CHECK(rdma::detail::cap_of(7) == 7);
  ASIO_CHECK(rdma::detail::cap_of(0) == 0);
  ASIO_CHECK(rdma::detail::cap_of(-1) == 0);
}

void defaults_are_derived_from_caps_with_library_limits()
{
  auto effective =
      rdma::detail::derive_effective_config(rdma::ibv_config_t{}, large_caps());

  ASIO_CHECK(effective.cqe_ == rdma::detail::default_cqe);
  ASIO_CHECK(effective.max_send_wr_ == rdma::detail::default_max_send_wr);
  ASIO_CHECK(effective.max_recv_wr_ == rdma::detail::default_max_recv_wr);
  ASIO_CHECK(effective.max_send_sge_ == rdma::detail::default_max_send_sge);
  ASIO_CHECK(effective.max_recv_sge_ == rdma::detail::default_max_recv_sge);
  ASIO_CHECK(effective.max_inline_data_ == 0);
  ASIO_CHECK(effective.inbound_read_limit_ == 4);
  ASIO_CHECK(effective.outbound_read_limit_ == 7);
  ASIO_CHECK(effective.cm_resolve_timeout_ms_ ==
             rdma::default_cm_resolve_timeout_ms);
}

void defaults_respect_small_device_caps()
{
  auto c = large_caps();
  c.max_cqe = 32;
  c.max_qp_wr = 17;
  c.max_sge = 2;
  c.max_qp_rd_atom = 1;
  c.max_qp_init_rd_atom = 3;

  auto effective =
      rdma::detail::derive_effective_config(rdma::ibv_config_t{}, c);

  ASIO_CHECK(effective.cqe_ == 32);
  ASIO_CHECK(effective.max_send_wr_ == 17);
  ASIO_CHECK(effective.max_recv_wr_ == 17);
  ASIO_CHECK(effective.max_send_sge_ == 2);
  ASIO_CHECK(effective.max_recv_sge_ == 2);
  ASIO_CHECK(effective.inbound_read_limit_ == 1);
  ASIO_CHECK(effective.outbound_read_limit_ == 3);
}

void explicit_user_values_are_preserved()
{
  rdma::ibv_config_t requested{};
  requested.cqe_ = 64;
  requested.max_send_wr_ = 33;
  requested.max_recv_wr_ = 44;
  requested.max_send_sge_ = 2;
  requested.max_recv_sge_ = 3;
  requested.max_inline_data_ = 11;
  requested.inbound_read_limit_ = 1;
  requested.outbound_read_limit_ = 2;
  requested.cm_resolve_timeout_ms_ = 9;
  requested.backlog_ = 10;

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
  ASIO_CHECK(effective.cm_resolve_timeout_ms_ ==
             requested.cm_resolve_timeout_ms_);
  ASIO_CHECK(effective.backlog_ == requested.backlog_);
}

void compatibility_checks_user_values_against_caps()
{
  auto c = large_caps();
  rdma::ibv_config_t valid{};
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
  "ibv/config_derive",
  ASIO_TEST_CASE(cap_of_clamps_non_positive_values)
  ASIO_TEST_CASE(defaults_are_derived_from_caps_with_library_limits)
  ASIO_TEST_CASE(defaults_respect_small_device_caps)
  ASIO_TEST_CASE(explicit_user_values_are_preserved)
  ASIO_TEST_CASE(compatibility_checks_user_values_against_caps)
)
