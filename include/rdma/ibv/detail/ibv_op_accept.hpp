#pragma once

#include <atomic>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/ibv/detail/ibv_op_cm.hpp"
#include "rdma/ibv/detail/ibv_ops_cm.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Server-side accept: the service issues rdma_accept on the (migrated) child
// cm_id; this op waits on that cm_id's own event channel for ESTABLISHED.
// Single-stage, so it never returns not_done after seeing an event.
//
// Mirrors connect's teardown arbitration: ESTABLISHED does CAS(connecting ->
// connected); if that fails, disconnect() won the race while the connection
// established -> this op (second actor) tears it down exactly once. Failures
// claim `closed` unless disconnect() beat us. See ibv_op_connect.hpp.
template <typename Handler, typename IoExecutor>
class ibv_accept_op final : public ibv_op_cm {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_accept_op);

private:
  std::atomic<connect_state>* state_;  // points at the connector's connect_state_
  native_cm_id_t* cm_id_;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ibv_accept_op(asio::error_code const& success_ec, native_cm_id_t* cm_id,
                std::atomic<connect_state>* state, Handler& handler,
                IoExecutor const& io_ex)
      // cm_id may be null if the connector was never opened (e.g. async_accept
      // before use_device); that path posts an immediate completion (do_perform
      // is never called), so a null channel is fine -- mirrors ibv_connect_op_base.
      : ibv_op_cm(success_ec, cm_id ? cm_id->channel : nullptr, &do_perform,
                  &ibv_accept_op::do_complete)
      , state_(state)
      , cm_id_(cm_id)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  // Terminal failure: claim `closed` unless disconnect() beat us. Returns true
  // if WE claimed it (keep the real error), false if disconnect() already won.
  bool claim_closed() {
    connect_state e = state_->load(std::memory_order_acquire);
    for (;;) {
      if (e == connect_state::closed) {
        return false;
      }
      if (state_->compare_exchange_weak(e, connect_state::closed,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        return true;
      }
    }
  }

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
      case RDMA_CM_EVENT_ESTABLISHED: {
        // Arbitration: connecting -> connected (single exit from connecting).
        connect_state e = connect_state::connecting;
        if (!op->state_->compare_exchange_strong(e, connect_state::connected,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
          // disconnect() won while we established -> second-actor teardown.
          asio::error_code ignored;
          detail::disconnect(op->cm_id_, ignored);
          op->ec_ = asio::error::operation_aborted;
        }
        break;
      }
      case RDMA_CM_EVENT_REJECTED:
        op->ec_ = asio::error::connection_refused;
        if (!op->claim_closed()) op->ec_ = asio::error::operation_aborted;
        break;
      case RDMA_CM_EVENT_CONNECT_ERROR:
        op->ec_ = asio::error::connection_aborted;
        if (!op->claim_closed()) op->ec_ = asio::error::operation_aborted;
        break;
      default:
        op->ec_ = asio::error::connection_aborted;
        if (!op->claim_closed()) op->ec_ = asio::error::operation_aborted;
        break;
    }
    return status::done;
  }

  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    ibv_accept_op* o = static_cast<ibv_accept_op*>(base);
    ptr p = {asio::detail::addressof(o->handler_), o, o};

    // Per-op cancel -> connector terminal (mirrors ibv_connect_op). Keyed on
    // operation_aborted; aborted <=> not established, so never clobbers connected.
    if (owner && o->ec_ == asio::error::operation_aborted) {
      o->state_->store(connect_state::closed, std::memory_order_release);
    }

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
