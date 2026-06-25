#ifndef RDMA_ND_DETAIL_IMPL_ND_SERVICE_VERBS_IPP
#define RDMA_ND_DETAIL_IMPL_ND_SERVICE_VERBS_IPP

#include "rdma/nd/detail/nd_service_verbs.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Create the QP from the impl's device/cq/config. Static: no io_context needed.
asio::error_code nd_verbs_service::create_qp(implementation_type& impl) {
  asio::error_code ec;
  if (impl.qp_) {
    ec = asio::error::already_open;
    ASIO_ERROR_LOCATION(ec);
    return ec;
  }
  if (!impl.device_ || !impl.device_->pd_ || !impl.cq_) {
    ec = rdma_errc::invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return ec;
  }
  auto const& eff = impl.config_;
  native_qp_init_attr qp_init_attr{
      .qp_context_ = nullptr,
      .rcq_ = impl.cq_,
      .icq_ = impl.cq_,
      .max_send_wr_ = eff.max_send_wr_,
      .max_recv_wr_ = eff.max_recv_wr_,
      .max_send_sge_ = eff.max_send_sge_,
      .max_recv_sge_ = eff.max_recv_sge_,
      .max_inline_data_ = eff.max_inline_data_,
  };
  impl.qp_.Attach(
      verbs_ops::create_qp(impl.device_->pd_.get(), qp_init_attr, ec));
  return ec;
}

}

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_SERVICE_VERBS_IPP
