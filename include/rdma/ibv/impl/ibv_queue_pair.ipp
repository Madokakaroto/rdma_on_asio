#ifndef RDMA_IBV_IMPL_IBV_QUEUE_PAIR_IPP
#define RDMA_IBV_IMPL_IBV_QUEUE_PAIR_IPP

#include "rdma/ibv/ibv_queue_pair.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma {

void ibv_queue_pair::bind(asio::io_context& io_ctx, asio::error_code& ec) {
  if (is_bound()) {
    ec = asio::error::already_open;
    return;
  }

  auto& dev_svc = asio::use_service<detail::ibv_device_service>(io_ctx);
  if (!dev_svc.is_registered()) {
    ec = make_error_code(rdma_errc::device_not_registered);
    return;
  }
  auto& io_svc =
      asio::use_service<detail::ibv_io_completion_service>(io_ctx);
  io_ctx_ = &io_ctx;
  impl_.device_ = dev_svc.get_device();
  impl_.cq_ = io_svc.get_cq();
  impl_.config_ = dev_svc.get_effective_config();
  impl_.poll_cq_ = nullptr;
  // Cache the verbs service once --the event-mode async_* path uses it per op.
  verbs_svc_ = &asio::use_service<detail::ibv_verbs_service>(io_ctx);
  // Start the shared-CQ poller (idempotent); the data plane never arms again.
  io_svc.ensure_poller_started();
  ec.clear();
}

void ibv_queue_pair::bind(ibv_completion_queue& cq, asio::error_code& ec) {
  if (is_bound()) {
    ec = asio::error::already_open;
    return;
  }

  io_ctx_ = nullptr;
  verbs_svc_ = nullptr;  // poll mode uses the static service entry points
  impl_.device_ = cq.device();
  impl_.cq_ = cq.native_handle();
  impl_.config_ = cq.effective_config();
  impl_.poll_cq_ = &cq;
  ec.clear();
}

}  // namespace asio::rdma

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_QUEUE_PAIR_IPP
