#ifndef RDMA_IBV_IMPL_IBV_OP_WAIT_DISCONNECT_IPP
#define RDMA_IBV_IMPL_IBV_OP_WAIT_DISCONNECT_IPP

#include "rdma/ibv/detail/ibv_op_wait_disconnect.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_wait_disconnect_op_base::ibv_wait_disconnect_op_base(
    asio::error_code const& success_ec, native_event_channel_t* channel,
    std::atomic<bool>* peer_closed, func_type complete_func)
    : ibv_op_cm(success_ec, channel, &do_perform, complete_func)
    , peer_closed_(peer_closed) {
}

ibv_wait_disconnect_op_base::status ibv_wait_disconnect_op_base::do_perform(
    asio::detail::reactor_op* base) {
  auto* op = static_cast<ibv_wait_disconnect_op_base*>(base);
  unique_rdma_cm_event_ptr event{};  // deleter acks on scope exit
  if (op->get_cm_event(event)) {
    return status::done;  // hard error pulling the event
  }
  if (!event) {
    return status::not_done;  // EAGAIN: no event yet, stay armed
  }
  switch (event->event) {
    case RDMA_CM_EVENT_DISCONNECTED:
      if (op->peer_closed_) {
        op->peer_closed_->store(true, std::memory_order_release);
      }
      op->ec_ = make_error_code(rdma_errc::disconnected);
      return status::done;
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
      if (op->peer_closed_) {
        op->peer_closed_->store(true, std::memory_order_release);
      }
      op->ec_ = make_error_code(rdma_errc::device_removed);
      return status::done;
    default:
      // TIMEWAIT_EXIT / ADDR_CHANGE / etc: acked above, keep waiting.
      return status::not_done;
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_OP_WAIT_DISCONNECT_IPP
