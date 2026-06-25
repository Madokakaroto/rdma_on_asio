#ifndef RDMA_ND_DETAIL_IMPL_ND_OP_BASE_IPP
#define RDMA_ND_DETAIL_IMPL_ND_OP_BASE_IPP

#include <cassert>

#include "rdma/nd/detail/nd_op_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

nd_op_base::status_t nd_op_base::resume_process(void* owner,
                                                asio::error_code& ec) {
  if (ec) {
    return status_t::completed;
  }
  auto const status = do_process(owner, ec);
  if (status_t::continuation == status) {
    assert(owner);
    auto* context = static_cast<asio::detail::win_iocp_io_context*>(owner);
    context->work_started();
    context->on_pending(this);
  }
  return status;
}

nd_op_base::status_t nd_op_base::do_process(void* owner, asio::error_code& ec) {
  assert(overlapped_);

  if (process_func_) {
    return process_func_(owner, this, ec);
  }
  return status_t::completed;
}

}

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_OP_BASE_IPP
