#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
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

// Client connect state machine: resolve_addr (issued by the service before the
// op is armed) -> ROUTE -> CONNECT -> ESTABLISHED. Each intermediate event
// issues the next rdma_cm call and returns not_done to stay armed on the fd.
//
// The op mirrors its progress into the connector's atomic connect_state_ via
// per-stage CAS so that disconnect() (any thread) can decide its teardown. The
// transition out of `connecting` (-> connected) is the single arbitration point:
// whoever acts second performs the one rdma_disconnect for the established
// connection. See docs/cancellation_stage1_object.md (design A).
class ibv_connect_op_base : public ibv_op_cm {
public:
  enum class stage_t {
    addr_resolve,
    addr_route,
    connect,
    done,
    error,
    begin = addr_resolve,
  };

protected:
  stage_t stage_;
  std::atomic<connect_state>* state_;  // points at the connector's connect_state_
  native_cm_id_t* cm_id_;
  int timeout_;
  // Outgoing private data must stay valid for the op's lifetime (caller's
  // responsibility, as in nd). rdma_connect copies it out of conn_param.
  void const* private_data_;
  std::uint8_t private_data_len_;
  // RDMA read/atomic negotiation, sourced from the effective config.
  std::uint8_t responder_resources_;
  std::uint8_t initiator_depth_;
  // Creates the QP on cm_id once it has a context (after ADDR_RESOLVED).
  ibv_create_qp_fn create_qp_;
  // Where to store the server's reply private data (from ESTABLISHED).
  ibv_pd_sink remote_pd_;

  ibv_connect_op_base(asio::error_code const& success_ec, native_cm_id_t* cm_id,
                      std::atomic<connect_state>* state, int timeout,
                      void const* private_data, std::size_t private_data_len,
                      std::uint8_t responder_resources,
                      std::uint8_t initiator_depth,
                      ibv_create_qp_fn create_qp, ibv_pd_sink remote_pd,
                      func_type complete_func)
      // cm_id may be null if auto-open failed; that path posts an immediate
      // completion (do_perform is never called), so a null channel is fine.
      : ibv_op_cm(success_ec, cm_id ? cm_id->channel : nullptr, &do_perform,
                  complete_func)
      , stage_(stage_t::begin)
      , state_(state)
      , cm_id_(cm_id)
      , timeout_(timeout)
      , private_data_(private_data)
      , private_data_len_(static_cast<std::uint8_t>(private_data_len))
      , responder_resources_(responder_resources)
      , initiator_depth_(initiator_depth)
      , create_qp_(std::move(create_qp))
      , remote_pd_(remote_pd) {
  }

private:
  // Forward-progress transition. Advances ONLY from the exact prior stage; the
  // op is the only forward writer and is reactor-serialized, so a failure can
  // only mean disconnect() already claimed `closed`.
  bool advance(connect_state from, connect_state to) {
    connect_state e = from;
    return state_->compare_exchange_strong(
        e, to, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // Reactor-side terminal failure: claim `closed` unless disconnect() beat us.
  // Returns true if WE claimed it (keep the real error), false if disconnect()
  // already won (caller overrides ec_ with operation_aborted).
  bool claim_closed() {
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

  // advance() lost the race to disconnect(): nothing was established, so just
  // report aborted (no teardown -- cm_id never reached ESTABLISHED).
  status aborted_by_disconnect() {
    this->ec_ = asio::error::operation_aborted;
    return status::done;
  }

  static status do_perform(asio::detail::reactor_op* base) {
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

  // NOTE: deliberately NO blanket `if (state_ == closed) bail` here. It would
  // wrongly skip the ESTABLISHED arbitration: if disconnect() set closed and
  // ESTABLISHED then arrives, do_process_connect MUST run the second-actor
  // teardown rather than silently abort and leak an established cm_id. Each
  // stage handles `closed` itself via advance()/CAS below.
  status do_process(unique_rdma_cm_event_ptr const& event) {
    switch (stage_) {
      case stage_t::addr_resolve:
        return do_process_addr_resolve(event);
      case stage_t::addr_route:
        return do_process_addr_route(event);
      case stage_t::connect:
        return do_process_connect(event);
      default:
        this->ec_ = make_error_code(ibv_errc::ext_invalid_device);
        return status::done;
    }
  }

  status do_process_addr_resolve(unique_rdma_cm_event_ptr const& event) {
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

  status do_process_addr_route(unique_rdma_cm_event_ptr const& event) {
    switch (event->event) {
      case RDMA_CM_EVENT_ROUTE_RESOLVED: {
        // About to issue rdma_connect -> move addr_route -> connecting first.
        if (!advance(connect_state::addr_route, connect_state::connecting)) {
          return aborted_by_disconnect();  // disconnect won: do not connect
        }
        rdma_conn_param param{};
        param.private_data = private_data_;
        param.private_data_len = private_data_len_;
        param.responder_resources = responder_resources_;
        param.initiator_depth = initiator_depth_;
        param.retry_count = 7;
        param.rnr_retry_count = 7;
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

  status do_process_connect(unique_rdma_cm_event_ptr const& event) {
    switch (event->event) {
      case RDMA_CM_EVENT_ESTABLISHED: {
        // THE arbitration: connecting -> connected. Exactly one of {this op,
        // disconnect()} wins the exit from `connecting`.
        connect_state e = connect_state::connecting;
        if (state_->compare_exchange_strong(e, connect_state::connected,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
          // We won: normal establishment. Capture the server's reply pd.
          auto const& cp = event->param.conn;
          if (remote_pd_.buf && remote_pd_.len && cp.private_data &&
              cp.private_data_len) {
            std::size_t n =
                (std::min)(static_cast<std::size_t>(cp.private_data_len),
                           remote_pd_.cap);
            std::memcpy(remote_pd_.buf, cp.private_data, n);
            *remote_pd_.len = n;
          }
          this->ec_ = asio::error_code{};
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
};

template <typename Handler, typename IoExecutor>
class ibv_connect_op final : public ibv_connect_op_base {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_connect_op);

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ibv_connect_op(asio::error_code const& success_ec, native_cm_id_t* cm_id,
                 std::atomic<connect_state>* state, int timeout,
                 void const* private_data, std::size_t private_data_len,
                 std::uint8_t responder_resources, std::uint8_t initiator_depth,
                 ibv_create_qp_fn create_qp, ibv_pd_sink remote_pd,
                 Handler& handler, IoExecutor const& io_ex)
      : ibv_connect_op_base(success_ec, cm_id, state, timeout, private_data,
                            private_data_len, responder_resources,
                            initiator_depth, std::move(create_qp), remote_pd,
                            &ibv_connect_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    ibv_connect_op* o = static_cast<ibv_connect_op*>(base);
    ptr p = {asio::detail::addressof(o->handler_), o, o};

    // Per-op cancel completes the op via the reactor (operation_aborted),
    // bypassing do_process's claim_closed. Mark the connector terminal so a later
    // async_connect is cleanly rejected (-> ext_connector_terminal). Keyed strictly
    // on operation_aborted so transient guard errors stay retryable; aborted <=>
    // not established, so this never clobbers a live `connected`.
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
