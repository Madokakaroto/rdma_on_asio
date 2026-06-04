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

// Device-selection compatibility lives in detail::is_valid_adapter (nd_device_impl.hpp),
// which the device manager uses. (The former is_config_compatible here was only used by the
// removed auto-discover use_device overload.)

}
