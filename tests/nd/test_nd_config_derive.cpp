#include <cassert>
#include <iostream>

#include <WinSock2.h>
#include <ws2tcpip.h>
#include "rdma/nd/detail/nd_config_derive.hpp"

namespace rdma = asio::rdma;

rdma::detail::native_context_config_t caps() {
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

void test_defaults_are_derived_from_caps_with_library_limits() {
  auto effective = rdma::detail::derive_effective_config(rdma::nd_config_t{},
                                                         caps());

  assert(effective.cqe_ == rdma::detail::default_cqe);
  assert(effective.max_send_wr_ == rdma::detail::default_max_send_wr);
  assert(effective.max_recv_wr_ == rdma::detail::default_max_recv_wr);
  assert(effective.max_send_sge_ == rdma::detail::default_max_send_sge);
  assert(effective.max_recv_sge_ == rdma::detail::default_max_recv_sge);
  assert(effective.max_inline_data_ == 96);
  assert(effective.inbound_read_limit_ == 4);
  assert(effective.outbound_read_limit_ == 7);

  std::cout << "[PASS] default config derives from caps and clamps queue limits\n";
}

void test_defaults_respect_small_device_caps() {
  auto c = caps();
  c.MaxCompletionQueueDepth = 32;
  c.MaxInitiatorQueueDepth = 17;
  c.MaxReceiveQueueDepth = 19;
  c.MaxInitiatorSge = 2;
  c.MaxReceiveSge = 3;

  auto effective =
      rdma::detail::derive_effective_config(rdma::nd_config_t{}, c);

  assert(effective.cqe_ == 32);
  assert(effective.max_send_wr_ == 17);
  assert(effective.max_recv_wr_ == 19);
  assert(effective.max_send_sge_ == 2);
  assert(effective.max_recv_sge_ == 3);

  std::cout << "[PASS] default config respects smaller device caps\n";
}

void test_user_values_are_preserved() {
  rdma::nd_config_t requested{};
  requested.cqe_ = 64;
  requested.max_send_wr_ = 33;
  requested.max_recv_wr_ = 44;
  requested.max_send_sge_ = 2;
  requested.max_recv_sge_ = 3;
  requested.max_inline_data_ = 11;
  requested.inbound_read_limit_ = 1;
  requested.outbound_read_limit_ = 2;

  auto effective = rdma::detail::derive_effective_config(requested, caps());

  assert(effective.cqe_ == requested.cqe_);
  assert(effective.max_send_wr_ == requested.max_send_wr_);
  assert(effective.max_recv_wr_ == requested.max_recv_wr_);
  assert(effective.max_send_sge_ == requested.max_send_sge_);
  assert(effective.max_recv_sge_ == requested.max_recv_sge_);
  assert(effective.max_inline_data_ == requested.max_inline_data_);
  assert(effective.inbound_read_limit_ == requested.inbound_read_limit_);
  assert(effective.outbound_read_limit_ == requested.outbound_read_limit_);

  std::cout << "[PASS] explicit user config values are preserved\n";
}

int main() {
  test_defaults_are_derived_from_caps_with_library_limits();
  test_defaults_respect_small_device_caps();
  test_user_values_are_preserved();

  std::cout << "\nAll nd_config_derive tests passed.\n";
  return 0;
}
