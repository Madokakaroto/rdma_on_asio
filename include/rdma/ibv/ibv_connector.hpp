#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "asio/associator.hpp"
#include "asio/buffer.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "rdma/ibv/ibv_queue_pair.hpp"
#include "rdma/ibv/detail/ibv_service_connector.hpp"
#include "rdma/detail/move_accept_handler.hpp"

namespace asio::rdma::detail {

// Adapts the full async_connect's void(ec, size_t reply_len) completion down to
// void(ec) for the no-reply convenience overload (drops reply_len). Associator-
// forwarding so the wrapped handler's cancellation_slot / allocator / executor
// still reach the underlying connect op.
template <typename Handler>
struct connect_drop_reply_adapter {
  Handler handler_;
  void operator()(asio::error_code ec, std::size_t /*reply_len*/) {
    std::move(handler_)(ec);
  }
};

}  // namespace asio::rdma::detail

namespace asio {

template <template <typename, typename> class Associator, typename Handler,
          typename Default>
struct associator<Associator,
                  asio::rdma::detail::connect_drop_reply_adapter<Handler>,
                  Default> : Associator<Handler, Default> {
  static typename Associator<Handler, Default>::type get(
      asio::rdma::detail::connect_drop_reply_adapter<Handler> const& a) noexcept {
    return Associator<Handler, Default>::get(a.handler_);
  }
  static auto get(
      asio::rdma::detail::connect_drop_reply_adapter<Handler> const& a,
      Default const& d) noexcept
      -> decltype(Associator<Handler, Default>::get(a.handler_, d)) {
    return Associator<Handler, Default>::get(a.handler_, d);
  }
};

}  // namespace asio

namespace asio::rdma {

// Control-plane connector over rdma_cm. Mirrors asio's basic_socket:
//   - open(port_space)            create the cm_id (like socket.open(protocol))
//   - assign(native_handle)       adopt a cm_id from the listener
//   - async_connect(qp, ...)      create the QP on the cm_id, then connect
//   - async_accept(qp, ...)       create the QP on the cm_id, then accept
// The connector owns the cm_id + QP; the queue_pair borrows the QP.
template <typename PortSpace>
class ibv_connector {
public:
  using service_type = detail::ibv_connector_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = detail::ibv_connector_handle_t;

  explicit ibv_connector(asio::io_context& io_ctx) : impl_(0, 0, io_ctx) {
  }

  // opening constructor (mirrors basic_socket(io_context, protocol)). Requires
  // use_device() on this io_context (config is centralized there).
  ibv_connector(asio::io_context& io_ctx, PortSpace const& ps)
      : impl_(0, 0, io_ctx) {
    asio::error_code ec;
    impl_.get_service().open(impl_.get_implementation(), ps.rdma_type(), ec);
    asio::detail::throw_error(ec);
  }

  ~ibv_connector() = default;
  ibv_connector(ibv_connector&&) = default;
  ibv_connector& operator=(ibv_connector&&) = default;
  ibv_connector(ibv_connector const&) = delete;
  ibv_connector& operator=(ibv_connector const&) = delete;

  // open: create the cm_id (client). port space carries the RDMA port space
  // (and family); mirrors basic_socket::open(protocol). Optional --async_connect
  // auto-opens if not already open.
  void open(PortSpace const& ps) {
    asio::error_code ec;
    open(ps, ec);
    asio::detail::throw_error(ec);
  }

  void open(PortSpace const& ps, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), ps.rdma_type(), ec);
  }

  // assign: adopt a cm_id handle produced by the listener (server side).
  void assign(native_connector_type&& handle) {
    asio::error_code ec;
    assign(std::move(handle), ec);
    asio::detail::throw_error(ec);
  }

  void assign(native_connector_type&& handle, asio::error_code& ec) {
    impl_.get_service().assign(impl_.get_implementation(), std::move(handle),
                               ec);
  }

  bool is_open() const noexcept {
    return impl_.get_service().is_open(impl_.get_implementation());
  }

  // Private data is exchanged via the connect/accept/get_connection buffers
  // (request -> async_get_connection's buffer; reply -> async_connect's reply
  // buffer); there is no separate get_remote_data() accessor.

  // No cancel(): object-level teardown -- including aborting an in-flight
  // async_connect/async_accept -- is disconnect(), which adapts to the
  // connection state and is thread-safe. (Only listener has cancel(), with
  // reusable accept semantics.) See docs/cancellation_stage1_object.md.

  // async connect: create the QP on the cm_id then connect. Sends
  // `outgoing_request` (copied; may be {}), receives the server's reply private
  // data into `incoming_reply` (may be {} to ignore it). The buffers' lifetime:
  // `outgoing_request` need not outlive the call (copied into the op);
  // `incoming_reply` must stay valid until completion (it is filled then).
  // handler(error_code, std::size_t reply_len) -- reply_len = bytes written into
  // incoming_reply (= min(rdma_cm-reported len, buffer size); the reported len is
  // transport-padded, NOT the sender's exact length).
  template <typename ConnectToken>
  auto async_connect(ibv_queue_pair& qp, endpoint_type const& endpoint,
                     asio::const_buffer outgoing_request,
                     asio::mutable_buffer incoming_reply,
                     ConnectToken&& token) {
    return asio::async_initiate<ConnectToken,
                                void(asio::error_code, std::size_t)>(
        [this, &qp, &endpoint, outgoing_request, incoming_reply](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_connect(
              impl_.get_implementation(), qp.make_create_qp_fn(), endpoint,
              outgoing_request, incoming_reply, handler, io_ex);
        },
        token);
  }

  // Convenience: connect without receiving the server's reply private data.
  // Completion is void(error_code) (no reply_len). handler(error_code)
  template <typename ConnectToken>
  auto async_connect(ibv_queue_pair& qp, endpoint_type const& endpoint,
                     asio::const_buffer outgoing_request,
                     ConnectToken&& token) {
    return asio::async_initiate<ConnectToken, void(asio::error_code)>(
        [this, &qp, &endpoint, outgoing_request](auto handler) {
          auto io_ex = impl_.get_executor();
          detail::connect_drop_reply_adapter<std::decay_t<decltype(handler)>>
              adapter{std::move(handler)};
          impl_.get_service().async_connect(
              impl_.get_implementation(), qp.make_create_qp_fn(), endpoint,
              outgoing_request, asio::mutable_buffer{}, adapter, io_ex);
        },
        token);
  }

  // async accept: create the QP on the cm_id then accept. handler(error_code)
  template <typename AcceptToken>
  auto async_accept(ibv_queue_pair& qp,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token) {
    return asio::async_initiate<AcceptToken, void(asio::error_code)>(
        [this, &qp, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_accept(impl_.get_implementation(),
                                           qp.make_create_qp_fn(),
                                           outgoing_private_data, handler,
                                           io_ex);
        },
        token);
  }

  // Convenience: accept without sending reply private data. handler(error_code)
  template <typename AcceptToken>
  auto async_accept(ibv_queue_pair& qp, AcceptToken&& token) {
    return async_accept(qp, asio::const_buffer{},
                        std::forward<AcceptToken>(token));
  }

  template <typename AcceptToken>
  auto async_accept(asio::io_context& qp_io,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token) {
    return asio::async_initiate<
        AcceptToken, void(asio::error_code, ibv_queue_pair)>(
        [this, outgoing_private_data](auto handler, asio::io_context* target) {
          ibv_queue_pair qp;
          asio::error_code bind_ec;
          qp.bind(*target, bind_ec);
          using adapter_type = detail::move_accept_handler<
              ibv_queue_pair, std::decay_t<decltype(handler)>>;
          adapter_type adapter{std::move(qp), std::move(handler)};
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_move_accept(
              impl_.get_implementation(), bind_ec, outgoing_private_data,
              adapter, io_ex);
        },
        token, &qp_io);
  }

  template <typename AcceptToken>
  auto async_accept(asio::io_context& qp_io, AcceptToken&& token) {
    return async_accept(qp_io, asio::const_buffer{},
                        std::forward<AcceptToken>(token));
  }

  template <typename AcceptToken>
  auto async_accept(ibv_completion_queue& cq,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token) {
    return asio::async_initiate<
        AcceptToken, void(asio::error_code, ibv_queue_pair)>(
        [this, outgoing_private_data](auto handler,
                                     ibv_completion_queue* target) {
          ibv_queue_pair qp;
          asio::error_code bind_ec;
          qp.bind(*target, bind_ec);
          using adapter_type = detail::move_accept_handler<
              ibv_queue_pair, std::decay_t<decltype(handler)>>;
          adapter_type adapter{std::move(qp), std::move(handler)};
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_move_accept(
              impl_.get_implementation(), bind_ec, outgoing_private_data,
              adapter, io_ex);
        },
        token, &cq);
  }

  template <typename AcceptToken>
  auto async_accept(ibv_completion_queue& cq, AcceptToken&& token) {
    return async_accept(cq, asio::const_buffer{},
                        std::forward<AcceptToken>(token));
  }

  // disconnect: synchronous, non-blocking, thread-safe, unified teardown
  // (mirrors socket shutdown/close which are sync; also subsumes cancel). Adapts
  // to the connection state: aborts an in-flight async_connect/async_accept with
  // operation_aborted, or tears down an established connection. For an
  // established connection rdma_disconnect transitions the local QP to ERROR and
  // flushes pending WRs; those pending send/recv complete on the CQ with
  // operation_aborted ASYNCHRONOUSLY, so disconnect() returning does not mean
  // they have already aborted. Callable from any thread (no strand needed); the
  // only contract is not destroying the connector concurrently. To be NOTIFIED
  // of a (peer) disconnect, use async_wait_disconnect.
  void disconnect() {
    asio::error_code ec;
    disconnect(ec);
    asio::detail::throw_error(ec);
  }

  void disconnect(asio::error_code& ec) {
    impl_.get_service().disconnect(impl_.get_implementation(), ec);
  }

  // Disconnect NOTIFICATION (on_disconnect): one-shot, completes when the
  // connection is disconnected. handler(error_code) -- rdma_errc::disconnected
  // on a peer disconnect, rdma_errc::device_removed if the local device was removed,
  // operation_aborted if you disconnect()ed yourself while waiting. If already
  // disconnected, completes immediately (level-triggered). Detects only GRACEFUL
  // (cm DISCONNECTED) teardown; ungraceful peer loss surfaces via data-plane op
  // errors (connection_reset) -- see disconnect_refactor_plan.md.
  template <typename WaitToken>
  auto async_wait_disconnect(WaitToken&& token) {
    return asio::async_initiate<WaitToken, void(asio::error_code)>(
        [this](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_wait_disconnect(impl_.get_implementation(),
                                                    handler, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
