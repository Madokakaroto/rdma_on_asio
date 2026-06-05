#pragma once

#include <utility>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "ibv/detail/ibv_op_cm.hpp"
#include "ibv/detail/ibv_ops_cm.hpp"
#include "ibv/ibv_error.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Disconnect-notification watcher (on_disconnect). A reactor_op that stays armed
// on the CM event channel fd until it observes a teardown event:
//   RDMA_CM_EVENT_DISCONNECTED   -> complete with ibv_errc::ext_disconnected
//   RDMA_CM_EVENT_DEVICE_REMOVAL -> complete with ibv_errc::ext_device_removed
// Other events (e.g. TIMEWAIT_EXIT) are acked (by the unique_ptr deleter) and
// ignored -- the op returns not_done to stay armed. On the terminal event it also
// sets the connector's disconnected_ flag so a subsequent async_wait_disconnect
// completes immediately (level-triggered).
//
// One-shot: completes once. Armed on demand by async_wait_disconnect (not always);
// if nothing is armed, queued CM events are drained+acked at teardown instead.
class ibv_wait_disconnect_op_base : public ibv_op_cm {
protected:
  bool* disconnected_;  // connector's flag, set on the terminal event

  ibv_wait_disconnect_op_base(asio::error_code const& success_ec,
                              native_event_channel_t* channel,
                              bool* disconnected, func_type complete_func)
      : ibv_op_cm(success_ec, channel, &do_perform, complete_func)
      , disconnected_(disconnected) {
  }

private:
  static status do_perform(asio::detail::reactor_op* base) {
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
        if (op->disconnected_) {
          *op->disconnected_ = true;
        }
        op->ec_ = make_error_code(ibv_errc::ext_disconnected);
        return status::done;
      case RDMA_CM_EVENT_DEVICE_REMOVAL:
        if (op->disconnected_) {
          *op->disconnected_ = true;
        }
        op->ec_ = make_error_code(ibv_errc::ext_device_removed);
        return status::done;
      default:
        // TIMEWAIT_EXIT / ADDR_CHANGE / etc: acked above, keep waiting.
        return status::not_done;
    }
  }
};

template <typename Handler, typename IoExecutor>
class ibv_wait_disconnect_op final : public ibv_wait_disconnect_op_base {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_wait_disconnect_op);

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ibv_wait_disconnect_op(asio::error_code const& success_ec,
                         native_event_channel_t* channel, bool* disconnected,
                         Handler& handler, IoExecutor const& io_ex)
      : ibv_wait_disconnect_op_base(success_ec, channel, disconnected,
                                    &ibv_wait_disconnect_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    ibv_wait_disconnect_op* o = static_cast<ibv_wait_disconnect_op*>(base);
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
