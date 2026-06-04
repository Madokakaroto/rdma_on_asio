#pragma once

#include <span>

#include "asio/buffer.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "nd/detail/nd_connector_service.hpp"
#include "nd/nd_queue_pair.hpp"

namespace asio::rdma {

// Control-plane connector over NetworkDirect. Mirrors ibv_connector / asio's
// basic_socket:
//   - open(port_space)            create the IND2Connector (like socket.open(protocol))
//   - assign(native_handle)       adopt a connector handle from the listener
//   - async_connect(qp, ...)      Bind + Connect using qp.native_handle()
//   - async_accept(qp, ...)       Accept using qp.native_handle()
// The connector owns the IND2Connector; the queue_pair owns the QP. The QP is
// borrowed at connect/accept time (unlike ibv where the connector creates it).
template <typename PortSpace>
class nd_connector {
public:
  using service_type = detail::nd_connector_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = detail::nd_connector_handle_t;

  explicit nd_connector(asio::io_context& io_ctx)
      : impl_(0, 0, io_ctx) {
  }

  // opening constructor (mirrors basic_socket(io_context, protocol)). Requires
  // use_device() on this io_context (config is centralized there).
  nd_connector(asio::io_context& io_ctx, PortSpace const& ps)
      : impl_(0, 0, io_ctx) {
    asio::error_code ec;
    impl_.get_service().open(impl_.get_implementation(), ps, ec);
    asio::detail::throw_error(ec);
  }

  ~nd_connector() = default;
  nd_connector(nd_connector&&) = default;
  nd_connector& operator=(nd_connector&&) = default;
  nd_connector(nd_connector const&) = delete;
  nd_connector& operator=(nd_connector const&) = delete;

  // open: create the IND2Connector (client). The PortSpace is accepted for API
  // parity with ibv (and possible future v4/v6 selection); ND has no explicit
  // RDMA port space. Optional -- async_connect auto-opens if not already open.
  void open(PortSpace const& ps) {
    asio::error_code ec;
    open(ps, ec);
    asio::detail::throw_error(ec);
  }

  void open(PortSpace const& ps, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), ps, ec);
  }

  // assign: adopt a connector handle produced by the listener (server side).
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

  // async connect: Bind + Connect using qp.native_handle(). handler(error_code)
  template <typename ConnectToken>
  auto async_connect(nd_queue_pair& qp, endpoint_type const& endpoint,
                     asio::const_buffer outgoing_private_data,
                     ConnectToken&& token) {
    return asio::async_initiate<ConnectToken, void(asio::error_code)>(
        [this, &qp, &endpoint, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_connect(
              impl_.get_implementation(), qp.native_handle(), endpoint,
              outgoing_private_data, handler, io_ex);
        },
        token);
  }

  // async accept: Accept using qp.native_handle(). handler(error_code)
  template <typename AcceptToken>
  auto async_accept(nd_queue_pair& qp,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token) {
    return asio::async_initiate<AcceptToken, void(asio::error_code)>(
        [this, &qp, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_accept(impl_.get_implementation(),
                                           qp.native_handle(),
                                           outgoing_private_data, handler,
                                           io_ex);
        },
        token);
  }

  // async disconnect: handler(error_code)
  template <typename DisconnectToken>
  auto async_disconnect(DisconnectToken&& token) {
    return asio::async_initiate<DisconnectToken, void(asio::error_code)>(
        [this](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_disconnect(
              impl_.get_implementation(), handler, io_ex);
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
