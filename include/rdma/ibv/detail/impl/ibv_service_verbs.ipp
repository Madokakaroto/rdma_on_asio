#ifndef RDMA_IBV_IMPL_IBV_SERVICE_VERBS_IPP
#define RDMA_IBV_IMPL_IBV_SERVICE_VERBS_IPP

#include "rdma/ibv/detail/ibv_service_verbs.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_verbs_service::ibv_verbs_service(asio::execution_context& context)
    : asio::detail::execution_context_service_base<ibv_verbs_service>(context)
    , ibv_service_base(context) {
}

// The native QP is owned by the connector; the CQ/device by the
// io_completion_service / completion_queue. Nothing to tear down here.
void ibv_verbs_service::shutdown() {}

// Create the QP on cm_id (called by the connector once cm_id has a context).
// Static: device/cq/config all come from the impl, so no io_context is needed.
asio::error_code ibv_verbs_service::create_qp(implementation_type& impl,
                                              native_cm_id_t* cm_id) {
  asio::error_code ec;
  if (!impl.device_ || !impl.device_->pd_ || !impl.cq_) {
    ec = make_error_code(rdma_errc::invalid_device);
    return ec;
  }
  auto const& eff = impl.config_;

  native_qp_init_attr_t attr{};
  attr.qp_context = nullptr;
  attr.send_cq = impl.cq_;
  attr.recv_cq = impl.cq_;
  attr.srq = nullptr;
  attr.cap.max_send_wr = eff.max_send_wr_;
  attr.cap.max_recv_wr = eff.max_recv_wr_;
  attr.cap.max_send_sge = eff.max_send_sge_;
  attr.cap.max_recv_sge = eff.max_recv_sge_;
  attr.cap.max_inline_data = 0;
  attr.qp_type = IBV_QPT_RC;
  attr.sq_sig_all = 1;

  if (verbs_ops::create_qp(cm_id, impl.device_->pd_.get(), attr, ec) == 0) {
    impl.qp_ = cm_id->qp;
  }
  return ec;
}

// event mode: a posted op needs nothing here --the io_completion_service's
// poller is already armed (started at queue_pair::bind) and self-perpetuating,
// so it will reap this op's CQE. Only an immediate completion (empty buffers /
// synchronous post error) needs scheduling onto the io_context.
void ibv_verbs_service::finish_event(implementation_type& /*impl*/,
                                     rdma_verbs_op_base* op, bool immediate) {
  if (immediate) [[unlikely]] {
    auto* complete = new ibv_complete_op(op);
    this->scheduler_.post_immediate_completion(complete, false);
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_SERVICE_VERBS_IPP
