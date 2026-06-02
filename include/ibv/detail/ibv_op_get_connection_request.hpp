#pragma once

#include <array>
#include <cstring>
#include <span>

#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "ibv/detail/ibv_op_cm.hpp"
#include "ibv/detail/ibv_ops_cm.hpp"
#include "ibv/ibv_error.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// max_private_data_size is defined in ibv_impl_types.hpp.

// Listener side: wait on the listener's event channel for CONNECT_REQUEST. On
// arrival, copy the client's private data, migrate the child cm_id (event->id)
// to its own fresh event channel, and deliver an ibv_connector_handle_t to the
// user. The event is acked (via unique_rdma_cm_event_ptr) only after the child
// id has been migrated out — per the ack-timing note in CLAUDE.md.
template <typename Handler, typename IoExecutor>
class ibv_get_connection_request_op final : public ibv_op_cm {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_get_connection_request_op);

  ibv_get_connection_request_op(asio::error_code const& success_ec,
                                native_event_channel_t* listen_channel,
                                Handler& handler, IoExecutor const& io_ex)
      : ibv_op_cm(success_ec, listen_channel, &do_perform,
                  &ibv_get_connection_request_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  ibv_connector_handle_t connector_handle_;
  std::array<std::byte, max_private_data_size> private_data_buf_{};
  std::size_t private_data_len_ = 0;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

  static status do_perform(asio::detail::reactor_op* base) {
    auto* op = static_cast<ibv_get_connection_request_op*>(base);

    unique_rdma_cm_event_ptr event{};
    if (op->get_cm_event(event)) {
      return status::done;
    }
    if (!event) {
      return status::not_done;  // EAGAIN: keep waiting for a request
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
      op->ec_ = asio::error::connection_aborted;
      return status::done;
    }

    // Copy the client's private data before the event is acked.
    auto const& cp = event->param.conn;
    if (cp.private_data && cp.private_data_len) {
      op->private_data_len_ =
          (std::min)(static_cast<std::size_t>(cp.private_data_len),
                     op->private_data_buf_.size());
      std::memcpy(op->private_data_buf_.data(), cp.private_data,
                  op->private_data_len_);
    }

    // The child id arrives on the listener's channel; give it its own channel
    // so its later events (ESTABLISHED/DISCONNECTED) are independent.
    native_event_channel_t* child_channel = create_event_channel(op->ec_);
    if (op->ec_) {
      return status::done;
    }
    // Take ownership of both before migrating, so they are released on any
    // error path (the op's destructor runs them) and moved out on success.
    op->connector_handle_.cm_channel_.reset(child_channel);
    op->connector_handle_.cm_id_.reset(event->id);

    if (migrate_id(event->id, child_channel, op->ec_) != 0) {
      return status::done;
    }
    return status::done;  // event acked here (unique_ptr deleter)
  }

  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    auto* o = static_cast<ibv_get_connection_request_op*>(base);
    asio::error_code ec = o->ec_;

    // Copy private data to a stack local: the op (and its buffer) is freed at
    // p.reset() below, but the span must stay valid for the upcall.
    std::array<std::byte, max_private_data_size> pd_buf = o->private_data_buf_;
    std::size_t const pd_len = o->private_data_len_;

    ibv_connector_handle_t handle = std::move(o->connector_handle_);

    ptr p = {asio::detail::addressof(o->handler_), o, o};

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

    ASIO_ERROR_LOCATION(ec);

    Handler handler(ASIO_MOVE_CAST(Handler)(o->handler_));
    p.h = asio::detail::addressof(handler);
    p.reset();

    if (owner) {
      std::span<const std::byte> private_data(pd_buf.data(), pd_len);
      asio::detail::fenced_block b(asio::detail::fenced_block::half);
      ASIO_HANDLER_INVOCATION_BEGIN((ec));
      handler(ec, std::move(handle), private_data);
      ASIO_HANDLER_INVOCATION_END;
    }
  }
};

}

#include "asio/detail/pop_options.hpp"
