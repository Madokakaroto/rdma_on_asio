#pragma once

#include <algorithm>
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

inline constexpr int default_cm_timeout_ms = 2000;

// Client connect state machine: resolve_addr (issued by the service before the
// op is armed) -> ROUTE -> CONNECT -> ESTABLISHED. Each intermediate event
// issues the next rdma_cm call and returns not_done to stay armed on the fd.
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
                      int timeout, void const* private_data,
                      std::size_t private_data_len,
                      std::uint8_t responder_resources,
                      std::uint8_t initiator_depth,
                      ibv_create_qp_fn create_qp, ibv_pd_sink remote_pd,
                      func_type complete_func)
      // cm_id may be null if auto-open failed; that path posts an immediate
      // completion (do_perform is never called), so a null channel is fine.
      : ibv_op_cm(success_ec, cm_id ? cm_id->channel : nullptr, &do_perform,
                  complete_func)
      , stage_(stage_t::begin)
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
        // cm_id->verbs is valid now: create the QP before routing/connecting.
        if (create_qp_) {
          this->ec_ = create_qp_(cm_id_);
          if (this->ec_) {
            stage_ = stage_t::error;
            return status::done;
          }
        }
        if (resolve_route(cm_id_, timeout_, this->ec_) == 0) {
          stage_ = stage_t::addr_route;
          return status::not_done;
        }
        stage_ = stage_t::error;
        return status::done;
      case RDMA_CM_EVENT_ADDR_ERROR:
        this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
        return status::done;
      default:
        this->ec_ = asio::error::connection_aborted;
        return status::done;
    }
  }

  status do_process_addr_route(unique_rdma_cm_event_ptr const& event) {
    switch (event->event) {
      case RDMA_CM_EVENT_ROUTE_RESOLVED: {
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
        stage_ = stage_t::error;
        return status::done;
      }
      case RDMA_CM_EVENT_ROUTE_ERROR:
        this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
        return status::done;
      default:
        this->ec_ = asio::error::connection_aborted;
        return status::done;
    }
  }

  status do_process_connect(unique_rdma_cm_event_ptr const& event) {
    switch (event->event) {
      case RDMA_CM_EVENT_ESTABLISHED: {
        // Capture the server's reply private data into the connector's buffer.
        auto const& cp = event->param.conn;
        if (remote_pd_.buf && remote_pd_.len && cp.private_data &&
            cp.private_data_len) {
          std::size_t n =
              (std::min)(static_cast<std::size_t>(cp.private_data_len),
                         remote_pd_.cap);
          std::memcpy(remote_pd_.buf, cp.private_data, n);
          *remote_pd_.len = n;
        }
        return status::done;
      }
      case RDMA_CM_EVENT_CONNECT_ERROR:
        this->ec_ = asio::error::connection_aborted;
        return status::done;
      case RDMA_CM_EVENT_UNREACHABLE:
        this->ec_ = asio::error::host_unreachable;
        return status::done;
      case RDMA_CM_EVENT_REJECTED:
        this->ec_ = asio::error::connection_refused;
        return status::done;
      default:
        this->ec_ = asio::error::connection_aborted;
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
                 int timeout, void const* private_data,
                 std::size_t private_data_len, std::uint8_t responder_resources,
                 std::uint8_t initiator_depth, ibv_create_qp_fn create_qp,
                 ibv_pd_sink remote_pd, Handler& handler,
                 IoExecutor const& io_ex)
      : ibv_connect_op_base(success_ec, cm_id, timeout, private_data,
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

// Disconnect: rdma_disconnect issued by the service, op waits for DISCONNECTED.
template <typename Handler, typename IoExecutor>
class ibv_disconnect_op final : public ibv_op_cm {
public:
  ASIO_DEFINE_HANDLER_PTR(ibv_disconnect_op);

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ibv_disconnect_op(asio::error_code const& success_ec, native_cm_id_t* cm_id,
                    Handler& handler, IoExecutor const& io_ex)
      : ibv_op_cm(success_ec, cm_id->channel, &do_perform,
                  &ibv_disconnect_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static status do_perform(asio::detail::reactor_op* base) {
    auto* op = static_cast<ibv_disconnect_op*>(base);
    unique_rdma_cm_event_ptr event{};
    if (op->get_cm_event(event)) {
      return status::done;
    }
    if (!event) {
      return status::not_done;
    }
    if (event->event != RDMA_CM_EVENT_DISCONNECTED) {
      op->ec_ = asio::error::connection_aborted;
    }
    return status::done;
  }

  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    ibv_disconnect_op* o = static_cast<ibv_disconnect_op*>(base);
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
