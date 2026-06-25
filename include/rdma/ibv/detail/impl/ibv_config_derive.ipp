#ifndef RDMA_IBV_IMPL_IBV_CONFIG_DERIVE_IPP
#define RDMA_IBV_IMPL_IBV_CONFIG_DERIVE_IPP

#include <algorithm>

#include "rdma/ibv/detail/ibv_config_derive.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_config_t derive_effective_config(ibv_config_t const& user_config,
                                     native_device_attr_t const& caps) {
  ibv_config_t effective = user_config;

  if (effective.cqe_ == 0) {
    effective.cqe_ = (std::min)(cap_of(caps.max_cqe), default_cqe);
  }
  if (effective.cq_poll_batch_ == 0) {
    effective.cq_poll_batch_ = static_cast<std::uint32_t>(default_cq_poll_batch);
  }
  if (effective.max_send_wr_ == 0) {
    effective.max_send_wr_ =
        (std::min)(cap_of(caps.max_qp_wr), default_max_send_wr);
  }
  if (effective.max_recv_wr_ == 0) {
    effective.max_recv_wr_ =
        (std::min)(cap_of(caps.max_qp_wr), default_max_recv_wr);
  }
  if (effective.max_send_sge_ == 0) {
    effective.max_send_sge_ =
        (std::min)(cap_of(caps.max_sge), default_max_send_sge);
  }
  if (effective.max_recv_sge_ == 0) {
    effective.max_recv_sge_ =
        (std::min)(cap_of(caps.max_sge), default_max_recv_sge);
  }
  // max_inline_data_ has no verbs device cap; 0 means "device default", which is
  // resolved when the QP is created. Leave the user value as-is.
  if (effective.inbound_read_limit_ == 0) {
    effective.inbound_read_limit_ = cap_of(caps.max_qp_rd_atom);
  }
  if (effective.outbound_read_limit_ == 0) {
    effective.outbound_read_limit_ = cap_of(caps.max_qp_init_rd_atom);
  }
  // CM resolve timeout is a policy (no device cap); 0 -> fixed default.
  if (effective.cm_resolve_timeout_ms_ == 0) {
    effective.cm_resolve_timeout_ms_ = default_cm_resolve_timeout_ms;
  }

  return effective;
}

bool is_config_compatible(ibv_config_t const& config,
                          native_device_attr_t const& caps) {
  if (config.cqe_ != 0 && config.cqe_ > cap_of(caps.max_cqe)) {
    return false;
  }
  if (config.max_send_wr_ != 0 && config.max_send_wr_ > cap_of(caps.max_qp_wr)) {
    return false;
  }
  if (config.max_recv_wr_ != 0 && config.max_recv_wr_ > cap_of(caps.max_qp_wr)) {
    return false;
  }
  if (config.max_send_sge_ != 0 && config.max_send_sge_ > cap_of(caps.max_sge)) {
    return false;
  }
  if (config.max_recv_sge_ != 0 && config.max_recv_sge_ > cap_of(caps.max_sge)) {
    return false;
  }
  // max_inline_data_ has no device cap to validate against; always compatible.
  if (config.inbound_read_limit_ != 0 &&
      config.inbound_read_limit_ > cap_of(caps.max_qp_rd_atom)) {
    return false;
  }
  if (config.outbound_read_limit_ != 0 &&
      config.outbound_read_limit_ > cap_of(caps.max_qp_init_rd_atom)) {
    return false;
  }
  return true;
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_CONFIG_DERIVE_IPP
