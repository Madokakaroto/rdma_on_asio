#pragma once

#include <span>

#include "asio/buffer.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_queue_pair.hpp"
#include "ibv/detail/ibv_connector_service.hpp"

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
  // (and family); mirrors basic_socket::open(protocol). Optional — async_connect
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
                               std::span<const std::byte>{}, ec);
  }

  bool is_open() const noexcept {
    return impl_.get_service().is_open(impl_.get_implementation());
  }

  // The peer's private data: the client's request data on the server side, the
  // server's reply data on the client side (after connect/accept completes).
  asio::const_buffer get_remote_data() const noexcept {
    return impl_.get_service().get_remote_data(impl_.get_implementation());
  }

  void cancel() {
    impl_.get_service().cancel(impl_.get_implementation());
  }

  // async connect: create the QP on the cm_id then connect. handler(error_code)
  template <typename ConnectToken>
  auto async_connect(ibv_queue_pair& qp, endpoint_type const& endpoint,
                     asio::const_buffer outgoing_private_data,
                     ConnectToken&& token) {
    return asio::async_initiate<ConnectToken, void(asio::error_code)>(
        [this, &qp, &endpoint, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_connect(
              impl_.get_implementation(), qp.make_create_qp_fn(), endpoint,
              outgoing_private_data, handler, io_ex);
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

  // disconnect: synchronous, non-blocking, abrupt teardown (mirrors socket
  // shutdown/close which are sync). rdma_disconnect transitions the local QP to
  // ERROR and flushes pending WRs; those pending send/recv complete on the CQ
  // with operation_aborted ASYNCHRONOUSLY, so disconnect() returning does not
  // mean they have already aborted. To be NOTIFIED of a (peer) disconnect, use
  // async_wait_disconnect.
  void disconnect() {
    asio::error_code ec;
    disconnect(ec);
    asio::detail::throw_error(ec);
  }

  void disconnect(asio::error_code& ec) {
    impl_.get_service().disconnect(impl_.get_implementation(), ec);
  }

  // Disconnect NOTIFICATION (on_disconnect): one-shot, completes when the
  // connection is disconnected. handler(error_code) -- ext_disconnected on a peer
  // disconnect, ext_device_removed if the local device was removed,
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

  // Internal: assign a handle + the peer's request private data (used by the
  // listener when delivering a connection). Not part of the user surface.
  void assign_with_private_data(native_connector_type&& handle,
                                std::span<const std::byte> remote_pd,
                                asio::error_code& ec) {
    impl_.get_service().assign(impl_.get_implementation(), std::move(handle),
                               remote_pd, ec);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
