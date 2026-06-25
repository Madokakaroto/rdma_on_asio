#ifndef RDMA_ND_IMPL_ND_CONFIG_DERIVE_IPP
#define RDMA_ND_IMPL_ND_CONFIG_DERIVE_IPP

#include <algorithm>

#include "rdma/nd/detail/nd_config_derive.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

nd_config_t derive_effective_config(nd_config_t const& user_config,
                                    native_context_config_t const& caps) {
  nd_config_t effective = user_config;

  if (effective.cqe_ == 0) {
    effective.cqe_ = (std::min)(caps.MaxCompletionQueueDepth, default_cqe);
  }
  if (effective.cq_poll_batch_ == 0) {
    effective.cq_poll_batch_ =
        static_cast<std::uint32_t>(default_cq_poll_batch);
  }
  if (effective.max_send_wr_ == 0) {
    effective.max_send_wr_ =
        (std::min)(caps.MaxInitiatorQueueDepth, default_max_send_wr);
  }
  if (effective.max_recv_wr_ == 0) {
    effective.max_recv_wr_ =
        (std::min)(caps.MaxReceiveQueueDepth, default_max_recv_wr);
  }
  if (effective.max_send_sge_ == 0) {
    effective.max_send_sge_ =
        (std::min)(caps.MaxInitiatorSge, default_max_send_sge);
  }
  if (effective.max_recv_sge_ == 0) {
    effective.max_recv_sge_ =
        (std::min)(caps.MaxReceiveSge, default_max_recv_sge);
  }
  if (effective.max_inline_data_ == 0) {
    effective.max_inline_data_ = caps.MaxInlineDataSize;
  }
  if (effective.inbound_read_limit_ == 0) {
    effective.inbound_read_limit_ = caps.MaxInboundReadLimit;
  }
  if (effective.outbound_read_limit_ == 0) {
    effective.outbound_read_limit_ = caps.MaxOutboundReadLimit;
  }

  return effective;
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_CONFIG_DERIVE_IPP
