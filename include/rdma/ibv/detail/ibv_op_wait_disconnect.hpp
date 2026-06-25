#pragma once

#include <atomic>
#include <utility>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/ibv/detail/ibv_op_cm.hpp"
#include "rdma/ibv/detail/ibv_ops_cm.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Disconnect-notification watcher (on_disconnect). A reactor_op that stays armed
// on the CM event channel fd until it observes a teardown event:
//   RDMA_CM_EVENT_DISCONNECTED   -> complete with rdma_errc::disconnected
//   RDMA_CM_EVENT_DEVICE_REMOVAL -> complete with rdma_errc::device_removed
// Other events (e.g. TIMEWAIT_EXIT) are acked (by the unique_ptr deleter) and
// ignored -- the op returns not_done to stay armed. On the terminal event it sets
// the connector's peer_closed_ latch so a subsequent async_wait_disconnect
// completes immediately (level-triggered).
//
// IMPORTANT: peer_closed_ is SEPARATE from connect_state_ (the teardown arbiter).
// A peer disconnect must NOT push connect_state_ to `closed`, or a later
// disconnect() would short-circuit and never flush local pending WRs. See
// docs/cancellation_stage1_object.md (design A.6).
//
// One-shot: completes once. Armed on demand by async_wait_disconnect (not always);
// if nothing is armed, queued CM events are drained+acked at teardown instead.
class ibv_wait_disconnect_op_base : public ibv_op_cm {
protected:
  std::atomic<bool>* peer_closed_;  // connector's latch, set on the terminal event

  ASIO_DECL ibv_wait_disconnect_op_base(asio::error_code const& success_ec,
                                        native_event_channel_t* channel,
                                        std::atomic<bool>* peer_closed,
                                        func_type complete_func);

private:
  ASIO_DECL static status do_perform(asio::detail::reactor_op* base);
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
                         native_event_channel_t* channel,
                         std::atomic<bool>* peer_closed, Handler& handler,
                         IoExecutor const& io_ex)
      : ibv_wait_disconnect_op_base(success_ec, channel, peer_closed,
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

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_op_wait_disconnect.ipp"
#endif
