#pragma once

#include <atomic>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Disconnect-notification watcher (on_disconnect). Issues IND2Connector::
// NotifyDisconnect; when that overlapped completes (connection disconnected),
// it sets the connector's peer_closed_ latch and upcalls handler with
// ext_disconnected. Mirrors ibv_wait_disconnect_op.
template <typename Handler, typename IoExecutor>
class nd_wait_disconnect_op final : public nd_op_base {
private:
  std::atomic<bool>* peer_closed_;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ASIO_DEFINE_HANDLER_PTR(nd_wait_disconnect_op);
  nd_wait_disconnect_op(IND2Connector* connector,
                        std::atomic<bool>* peer_closed,
                        Handler& handler, const IoExecutor& io_ex)
      : nd_op_base(connector, &nd_op_base::default_process,
                   &nd_wait_disconnect_op::do_complete)
      , peer_closed_(peer_closed)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    nd_wait_disconnect_op* o = static_cast<nd_wait_disconnect_op*>(base);
    if (o->peer_closed_) {
      o->peer_closed_->store(true, std::memory_order_release);
    }
    asio::error_code ec = make_error_code(nd_errc::ext_disconnected);

    ptr p = {asio::detail::addressof(o->handler_), o, o};

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

    ASIO_ERROR_LOCATION(ec);

    asio::detail::binder1<Handler, asio::error_code> handler(o->handler_, ec);
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
