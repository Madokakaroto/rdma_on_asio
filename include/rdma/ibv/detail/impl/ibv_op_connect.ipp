#ifndef RDMA_IBV_DETAIL_IMPL_IBV_OP_CONNECT_IPP
#define RDMA_IBV_DETAIL_IMPL_IBV_OP_CONNECT_IPP

#include <algorithm>
#include <cstring>
#include <utility>

#include "rdma/ibv/detail/ibv_op_connect.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_connect_op_base::ibv_connect_op_base(
    asio::error_code const& success_ec, native_cm_id_t* cm_id,
    std::atomic<connect_state>* state, int timeout, void const* request,
    std::size_t request_len, void* reply_buf, std::size_t reply_cap,
    std::uint8_t responder_resources, std::uint8_t initiator_depth,
    std::uint8_t rnr_retry, std::uint8_t min_rnr_timer,
    ibv_create_qp_fn create_qp, func_type complete_func)
    // cm_id may be null if auto-open failed; that path posts an immediate
    // completion (do_perform is never called), so a null channel is fine.
    : ibv_op_cm(success_ec, cm_id ? cm_id->channel : nullptr, &do_perform,
                complete_func)
    , stage_(stage_t::begin)
    , state_(state)
    , cm_id_(cm_id)
    , timeout_(timeout)
    , request_len_(static_cast<std::uint8_t>(
          (std::min)(request_len, max_outgoing_private_data)))
    , reply_buf_(reply_buf)
    , reply_cap_(reply_cap)
    , responder_resources_(responder_resources)
    , initiator_depth_(initiator_depth)
    , rnr_retry_(rnr_retry)
    , min_rnr_timer_(min_rnr_timer)
    , create_qp_(std::move(create_qp)) {
  if (request_len_) {
    std::memcpy(request_buf_.data(), request, request_len_);
  }
}

bool ibv_connect_op_base::advance(connect_state from, connect_state to) {
  connect_state e = from;
  return state_->compare_exchange_strong(
      e, to, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool ibv_connect_op_base::claim_closed() {
  connect_state e = state_->load(std::memory_order_acquire);
  for (;;) {
    if (e == connect_state::closed) {
      return false;  // disconnect won
    }
    if (state_->compare_exchange_weak(e, connect_state::closed,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      return true;
    }
  }
}

ibv_connect_op_base::status ibv_connect_op_base::aborted_by_disconnect() {
  this->ec_ = asio::error::operation_aborted;
  return status::done;
}

ibv_connect_op_base::status ibv_connect_op_base::do_perform(asio::detail::reactor_op* base) {
  auto* op = static_cast<ibv_connect_op_base*>(base);

  unique_rdma_cm_event_ptr event{};
  if (op->get_cm_event(event)) {
    return status::done;  // hard error pulling the event
  }
  if (!event) {
    return status::not_done;  // EAGAIN: no event yet, stay armed
  }
  return op->do_process(event);
}

ibv_connect_op_base::status ibv_connect_op_base::do_process(unique_rdma_cm_event_ptr const& event) {
  switch (stage_) {
    case stage_t::addr_resolve:
      return do_process_addr_resolve(event);
    case stage_t::addr_route:
      return do_process_addr_route(event);
    case stage_t::connect:
      return do_process_connect(event);
    default:
      this->ec_ = make_error_code(rdma_errc::invalid_device);
      return status::done;
  }
}

ibv_connect_op_base::status ibv_connect_op_base::do_process_addr_resolve(
    unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
      // About to create the QP + resolve the route -> move addr_resolve ->
      // addr_route first. If disconnect() already claimed closed, bail before
      // doing any work (no QP created, no route resolved).
      if (!advance(connect_state::addr_resolve, connect_state::addr_route)) {
        return aborted_by_disconnect();
      }
      // cm_id->verbs is valid now: create the QP before routing/connecting.
      if (create_qp_) {
        this->ec_ = create_qp_(cm_id_);
        if (this->ec_) {
          if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
          return status::done;
        }
      }
      if (resolve_route(cm_id_, timeout_, this->ec_) == 0) {
        stage_ = stage_t::addr_route;
        return status::not_done;
      }
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_ADDR_ERROR:
      this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}

ibv_connect_op_base::status ibv_connect_op_base::do_process_addr_route(
    unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ROUTE_RESOLVED: {
      // About to issue rdma_connect -> move addr_route -> connecting first.
      if (!advance(connect_state::addr_route, connect_state::connecting)) {
        return aborted_by_disconnect();  // disconnect won: do not connect
      }
      rdma_conn_param param{};
      param.private_data = request_len_ ? request_buf_.data() : nullptr;
      param.private_data_len = request_len_;
      param.responder_resources = responder_resources_;
      param.initiator_depth = initiator_depth_;
      param.retry_count = 7;
      param.rnr_retry_count = rnr_retry_;
      if (connect(cm_id_, &param, this->ec_) == 0) {
        stage_ = stage_t::connect;
        return status::not_done;
      }
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    }
    case RDMA_CM_EVENT_ROUTE_ERROR:
      this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}

ibv_connect_op_base::status ibv_connect_op_base::do_process_connect(
    unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ESTABLISHED: {
      // THE arbitration: connecting -> connected. Exactly one of {this op,
      // disconnect()} wins the exit from `connecting`.
      connect_state e = connect_state::connecting;
      if (state_->compare_exchange_strong(e, connect_state::connected,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        // We won: normal establishment. Copy the server's reply pd into the
        // caller's reply buffer; reply_len_ is reported via the completion.
        auto const& cp = event->param.conn;
        if (reply_buf_ && reply_cap_ && cp.private_data && cp.private_data_len) {
          reply_len_ =
              (std::min)(static_cast<std::size_t>(cp.private_data_len),
                         reply_cap_);
          std::memcpy(reply_buf_, cp.private_data, reply_len_);
        }
        // QP is now RTS; apply min_rnr_timer here -- rdma_cm leaves it at its
        // ~655 ms default, which stalls senders on a recv-window underrun. A
        // failure is reported as the connect error (success returns an empty
        // ec, which clears the success we set above).
        native_qp_attr_t qp_attr{};
        qp_attr.min_rnr_timer = min_rnr_timer_;
        this->ec_ =
            verbs_ops::modify_qp(cm_id_->qp, &qp_attr, IBV_QP_MIN_RNR_TIMER);
      }
      else {
        // e == closed: disconnect() won while we awaited ESTABLISHED. It saw
        // `connecting` and did NOT rdma_disconnect. The connection DID
        // establish -> we (the second actor) tear it down exactly once.
        asio::error_code ignored;
        detail::disconnect(cm_id_, ignored);
        this->ec_ = asio::error::operation_aborted;
      }
      return status::done;
    }
    case RDMA_CM_EVENT_CONNECT_ERROR:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_UNREACHABLE:
      this->ec_ = asio::error::host_unreachable;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_REJECTED:
      this->ec_ = asio::error::connection_refused;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_DETAIL_IMPL_IBV_OP_CONNECT_IPP
