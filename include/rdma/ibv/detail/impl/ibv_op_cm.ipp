#ifndef RDMA_IBV_IMPL_IBV_OP_CM_IPP
#define RDMA_IBV_IMPL_IBV_OP_CM_IPP

#include "rdma/ibv/detail/ibv_op_cm.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

asio::error_code ibv_op_cm::get_cm_event(unique_rdma_cm_event_ptr& event_ptr) {
  native_cm_event_t* event = nullptr;
  int const rc = detail::get_cm_event(channel_, &event, this->ec_);
  if (rc == 0) {
    event_ptr.reset(event);
  }
  return this->ec_;
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_OP_CM_IPP
