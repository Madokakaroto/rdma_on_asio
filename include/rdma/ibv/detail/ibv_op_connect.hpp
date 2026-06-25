#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/ibv/detail/ibv_op_cm.hpp"
#include "rdma/ibv/detail/ibv_ops_cm.hpp"
#include "rdma/ibv/detail/ibv_ops_verbs.hpp"
#include "rdma/ibv/ibv_error.hpp"
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
  // Outgoing request private data, COPIED into the op at construction (the caller
  // need not keep its buffer alive: rdma_connect is issued asynchronously after
  // ADDR/ROUTE resolve). Capped at max_outgoing_private_data; the service rejects
  // oversize (rdma_errc::private_data_too_large) before arming, so the copy never truncates
  // a value the caller expected to go out whole.
  std::array<std::byte, max_outgoing_private_data> request_buf_{};
  std::uint8_t request_len_;
  // Incoming reply (server's accept private_data) -> the caller's mutable buffer,
  // filled on ESTABLISHED. reply_len_ (bytes written = min(rdma_cm-reported, cap))
  // is reported through the completion handler.
  void* reply_buf_;
  std::size_t reply_cap_;
  std::size_t reply_len_ = 0;
  // RDMA read/atomic negotiation + RNR params, sourced from the effective config.
  // rnr_retry_ -> rdma_conn_param.rnr_retry_count; min_rnr_timer_ is applied via
  // ibv_modify_qp on ESTABLISHED (rdma_cm does not expose it through conn_param).
  std::uint8_t responder_resources_;
  std::uint8_t initiator_depth_;
  std::uint8_t rnr_retry_;
  std::uint8_t min_rnr_timer_;
  // Creates the QP on cm_id once it has a context (after ADDR_RESOLVED).
  ibv_create_qp_fn create_qp_;

  ASIO_DECL ibv_connect_op_base(asio::error_code const& success_ec,
                                native_cm_id_t* cm_id,
                                std::atomic<connect_state>* state, int timeout,
                                void const* request, std::size_t request_len,
                                void* reply_buf, std::size_t reply_cap,
                                std::uint8_t responder_resources,
                                std::uint8_t initiator_depth,
                                std::uint8_t rnr_retry,
                                std::uint8_t min_rnr_timer,
                                ibv_create_qp_fn create_qp,
                                func_type complete_func);

private:
  // Forward-progress transition. Advances ONLY from the exact prior stage; the
  // op is the only forward writer and is reactor-serialized, so a failure can
  // only mean disconnect() already claimed `closed`.
  ASIO_DECL bool advance(connect_state from, connect_state to);

  // Reactor-side terminal failure: claim `closed` unless disconnect() beat us.
  // Returns true if WE claimed it (keep the real error), false if disconnect()
  // already won (caller overrides ec_ with operation_aborted).
  ASIO_DECL bool claim_closed();

  // advance() lost the race to disconnect(): nothing was established, so just
  // report aborted (no teardown -- cm_id never reached ESTABLISHED).
  ASIO_DECL status aborted_by_disconnect();

  ASIO_DECL static status do_perform(asio::detail::reactor_op* base);

  // NOTE: deliberately NO blanket `if (state_ == closed) bail` here. It would
  // wrongly skip the ESTABLISHED arbitration: if disconnect() set closed and
  // ESTABLISHED then arrives, do_process_connect MUST run the second-actor
  // teardown rather than silently abort and leak an established cm_id. Each
  // stage handles `closed` itself via advance()/CAS below.
  ASIO_DECL status do_process(unique_rdma_cm_event_ptr const& event);

  ASIO_DECL status do_process_addr_resolve(
      unique_rdma_cm_event_ptr const& event);

  ASIO_DECL status do_process_addr_route(
      unique_rdma_cm_event_ptr const& event);

  ASIO_DECL status do_process_connect(unique_rdma_cm_event_ptr const& event);
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
                 void const* request, std::size_t request_len, void* reply_buf,
                 std::size_t reply_cap, std::uint8_t responder_resources,
                 std::uint8_t initiator_depth, std::uint8_t rnr_retry,
                 std::uint8_t min_rnr_timer, ibv_create_qp_fn create_qp,
                 Handler& handler, IoExecutor const& io_ex)
      : ibv_connect_op_base(success_ec, cm_id, state, timeout, request,
                            request_len, reply_buf, reply_cap,
                            responder_resources, initiator_depth, rnr_retry,
                            min_rnr_timer, std::move(create_qp),
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
    // async_connect is cleanly rejected (-> rdma_errc::connector_terminal). Keyed strictly
    // on operation_aborted so transient guard errors stay retryable; aborted <=>
    // not established, so this never clobbers a live `connected`.
    if (owner && o->ec_ == asio::error::operation_aborted) {
      o->state_->store(connect_state::closed, std::memory_order_release);
    }

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

    ASIO_ERROR_LOCATION(o->ec_);

    asio::detail::binder2<Handler, asio::error_code, std::size_t> handler(
        o->handler_, o->ec_, o->reply_len_);
    p.h = asio::detail::addressof(handler.handler_);
    p.reset();

    if (owner) {
      asio::detail::fenced_block b(asio::detail::fenced_block::half);
      ASIO_HANDLER_INVOCATION_BEGIN((handler.arg1_, handler.arg2_));
      w.complete(handler, handler.handler_);
      ASIO_HANDLER_INVOCATION_END;
    }
  }
};

}

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_op_connect.ipp"
#endif

