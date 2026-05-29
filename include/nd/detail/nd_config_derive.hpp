#pragma once

#include "nd/nd_types.hpp"
#include "nd/detail/nd_impl_types.hpp"

namespace asio::rdma::detail {

inline constexpr size_type default_cqe = 4096;
inline constexpr size_type default_max_send_wr = 128;
inline constexpr size_type default_max_recv_wr = 128;
inline constexpr size_type default_max_send_sge = 4;
inline constexpr size_type default_max_recv_sge = 4;

inline nd_config_t derive_effective_config(nd_config_t const& user_config,
                                           native_context_config_t const& caps) {
  nd_config_t effective = user_config;

  if (effective.cqe_ == 0) {
    effective.cqe_ = (std::min)(caps.MaxCompletionQueueDepth, default_cqe);
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

inline bool is_config_compatible(nd_config_t const& config,
                                 native_context_config_t const& caps) {
  if (config.cqe_ != 0 && config.cqe_ > caps.MaxCompletionQueueDepth) {
    return false;
  }
  if (config.max_send_wr_ != 0 &&
      config.max_send_wr_ > caps.MaxInitiatorQueueDepth) {
    return false;
  }
  if (config.max_recv_wr_ != 0 &&
      config.max_recv_wr_ > caps.MaxReceiveQueueDepth) {
    return false;
  }
  if (config.max_send_sge_ != 0 &&
      config.max_send_sge_ > caps.MaxInitiatorSge) {
    return false;
  }
  if (config.max_recv_sge_ != 0 && config.max_recv_sge_ > caps.MaxReceiveSge) {
    return false;
  }
  if (config.max_inline_data_ != 0 &&
      config.max_inline_data_ > caps.MaxInlineDataSize) {
    return false;
  }
  if (config.inbound_read_limit_ != 0 &&
      config.inbound_read_limit_ > caps.MaxInboundReadLimit) {
    return false;
  }
  if (config.outbound_read_limit_ != 0 &&
      config.outbound_read_limit_ > caps.MaxOutboundReadLimit) {
    return false;
  }
  return true;
}

}
