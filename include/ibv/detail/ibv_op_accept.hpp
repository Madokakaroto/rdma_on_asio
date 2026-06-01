#pragma once

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "ibv/detail/ibv_op_cm.hpp"
#include "ibv/ibv_error.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Server-side accept: the service issues rdma_accept on the (migrated) child
// cm_id; this op waits on that cm_id's own event channel for ESTABLISHED.
// Single-stage, so it never returns not_done after seeing an event.
template <typename Handler, typename IoExecutor>
class ibv_accept_op final : public ibv_op_cm {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_accept_op);

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ibv_accept_op(asio::error_code const& success_ec, native_cm_id_t* cm_id,
                Handler& handler, IoExecutor const& io_ex)
      : ibv_op_cm(success_ec, cm_id->channel, &do_perform,
                  &ibv_accept_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static status do_perform(asio::detail::reactor_op* base) {
    auto* op = static_cast<ibv_accept_op*>(base);
    unique_rdma_cm_event_ptr event{};
    if (op->get_cm_event(event)) {
      return status::done;
    }
    if (!event) {
      return status::not_done;
    }
    switch (event->event) {
      case RDMA_CM_EVENT_ESTABLISHED:
        break;
      case RDMA_CM_EVENT_REJECTED:
        op->ec_ = asio::error::connection_refused;
        break;
      case RDMA_CM_EVENT_CONNECT_ERROR:
        op->ec_ = asio::error::connection_aborted;
        break;
      default:
        op->ec_ = asio::error::connection_aborted;
        break;
    }
    return status::done;
  }

  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    ibv_accept_op* o = static_cast<ibv_accept_op*>(base);
    ptr p = {asio::detail::addressof(o->handler_), o, o};

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

    ASIO_ERROR_LOCATION(o->ec_);

    asio::detail::binder1<Handler, asio::error_code> handler(o->handler_,
                                                             o->ec_);
    p.h = asio::detail::addressof(handler.handler_);
    p.reset();

    if (owner) {
      asio::detail::fenced_block b(asio::detail::fenced_block::half);
      ASIO_HANDLER_INVOCATION_BEGIN((handler.arg1_));
      w.complete(handler, handler.handler_);
      ASIO_HANDLER_INVOCATION_END;
    }
  }
};

}

#include "asio/detail/pop_options.hpp"
