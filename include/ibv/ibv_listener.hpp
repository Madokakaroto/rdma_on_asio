#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#include "asio/associator.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_connector.hpp"
#include "ibv/detail/ibv_service_listener.hpp"

namespace asio::rdma::detail {

// Associator-forwarding completion adapters for listener::async_get_connection.
// The service op completes with (ec, native handle, private_data); the public
// API completes with (ec, connector) [return form] or (ec) [fill form] by
// building/assigning a connector. Forwarding the wrapped handler's associators
// (executor / cancellation_slot / allocator) is essential: a plain lambda would
// DROP the cancellation_slot, so cancel_after / co_spawn-cancel on
// async_get_connection would silently not cancel the underlying op (Stage 2).
template <typename Handler, typename ConnectorType>
struct get_connection_adapter {
  Handler handler_;
  asio::io_context* io_ctx_;
  asio::mutable_buffer request_;  // caller's buffer for the client's request pd

  void operator()(asio::error_code ec, ibv_connector_handle_t handle,
                  std::span<const std::byte> pd) {
    ConnectorType conn(*io_ctx_);
    std::size_t n = 0;
    if (!ec) {
      n = (std::min)(pd.size(), request_.size());
      if (n) std::memcpy(request_.data(), pd.data(), n);
      asio::error_code aec;
      conn.assign(std::move(handle), aec);
      if (aec) ec = aec;
    }
    std::move(handler_)(ec, std::move(conn), n);
  }
};

template <typename Handler, typename ConnectorType>
struct get_connection_fill_adapter {
  Handler handler_;
  ConnectorType* conn_;
  asio::mutable_buffer request_;  // caller's buffer for the client's request pd

  void operator()(asio::error_code ec, ibv_connector_handle_t handle,
                  std::span<const std::byte> pd) {
    std::size_t n = 0;
    if (!ec) {
      n = (std::min)(pd.size(), request_.size());
      if (n) std::memcpy(request_.data(), pd.data(), n);
      asio::error_code aec;
      conn_->assign(std::move(handle), aec);
      if (aec) ec = aec;
    }
    std::move(handler_)(ec, n);
  }
};

}  // namespace asio::rdma::detail

namespace asio {

template <template <typename, typename> class Associator, typename Handler,
          typename ConnectorType, typename Default>
struct associator<
    Associator,
    asio::rdma::detail::get_connection_adapter<Handler, ConnectorType>, Default>
    : Associator<Handler, Default> {
  static typename Associator<Handler, Default>::type get(
      asio::rdma::detail::get_connection_adapter<Handler, ConnectorType> const& a)
      noexcept {
    return Associator<Handler, Default>::get(a.handler_);
  }
  static auto get(
      asio::rdma::detail::get_connection_adapter<Handler, ConnectorType> const& a,
      Default const& d) noexcept
      -> decltype(Associator<Handler, Default>::get(a.handler_, d)) {
    return Associator<Handler, Default>::get(a.handler_, d);
  }
};

template <template <typename, typename> class Associator, typename Handler,
          typename ConnectorType, typename Default>
struct associator<
    Associator,
    asio::rdma::detail::get_connection_fill_adapter<Handler, ConnectorType>,
    Default> : Associator<Handler, Default> {
  static typename Associator<Handler, Default>::type get(
      asio::rdma::detail::get_connection_fill_adapter<Handler, ConnectorType> const&
          a) noexcept {
    return Associator<Handler, Default>::get(a.handler_);
  }
  static auto get(
      asio::rdma::detail::get_connection_fill_adapter<Handler, ConnectorType> const&
          a,
      Default const& d) noexcept
      -> decltype(Associator<Handler, Default>::get(a.handler_, d)) {
    return Associator<Handler, Default>::get(a.handler_, d);
  }
};

}  // namespace asio

namespace asio::rdma {

// Control-plane listener over rdma_cm. Mirrors asio's acceptor:
//   - open(port_space) / bind(port) / listen(backlog)
//   - async_get_connection()        -> a new connector (peer connection)
//   - async_get_connection(conn)    -> fill a pre-built connector
template <typename PortSpace>
class ibv_listener {
public:
  using service_type = detail::ibv_listener_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using connector_type = ibv_connector<PortSpace>;
  using native_connector_type = detail::ibv_connector_handle_t;

  explicit ibv_listener(asio::io_context& io_ctx) : impl_(0, 0, io_ctx) {
  }

  ~ibv_listener() = default;
  ibv_listener(ibv_listener&&) = default;
  ibv_listener& operator=(ibv_listener&&) = default;
  ibv_listener(ibv_listener const&) = delete;
  ibv_listener& operator=(ibv_listener const&) = delete;

  // Requires use_device() on this io_context (config is centralized there).
  void open(PortSpace const& ps) {
    asio::error_code ec;
    open(ps, ec);
    asio::detail::throw_error(ec);
  }

  void open(PortSpace const& ps, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), ps, ec);
  }

  void bind(asio::ip::port_type port) {
    asio::error_code ec;
    bind(port, ec);
    asio::detail::throw_error(ec);
  }

  void bind(asio::ip::port_type port, asio::error_code& ec) {
    impl_.get_service().bind(impl_.get_implementation(), port, ec);
  }

  void listen(int backlog = 128) {
    asio::error_code ec;
    listen(backlog, ec);
    asio::detail::throw_error(ec);
  }

  void listen(int backlog, asio::error_code& ec) {
    impl_.get_service().listen(impl_.get_implementation(), backlog, ec);
  }

  bool is_open() const noexcept {
    return impl_.get_service().is_open(impl_.get_implementation());
  }

  void cancel() {
    impl_.get_service().cancel(impl_.get_implementation());
  }

  // Return form: handler(error_code, connector_type, std::size_t request_len).
  // The connector is built on the listener's io_context with the adopted cm_id;
  // the client's request private data is copied into `request` (request_len =
  // bytes written = min(rdma_cm-reported len, buffer size); transport-padded, NOT
  // the sender's exact length). `request` must stay valid until completion; pass
  // {} to ignore the request data.
  template <typename AcceptToken>
  auto async_get_connection(asio::mutable_buffer request, AcceptToken&& token) {
    return asio::async_initiate<
        AcceptToken, void(asio::error_code, connector_type, std::size_t)>(
        [this, request](auto handler) {
          auto io_ex = impl_.get_executor();
          detail::get_connection_adapter<std::decay_t<decltype(handler)>,
                                         connector_type>
              adapter{std::move(handler), &io_ex.context(), request};
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), adapter, io_ex);
        },
        token);
  }

  // Fill form: handler(error_code, std::size_t request_len); assigns the
  // connection into a pre-built (empty) connector -- lets the caller pick its
  // io_context. `request` semantics as above.
  template <typename AcceptToken>
  auto async_get_connection(connector_type& conn, asio::mutable_buffer request,
                            AcceptToken&& token) {
    return asio::async_initiate<AcceptToken,
                                void(asio::error_code, std::size_t)>(
        [this, &conn, request](auto handler) {
          auto io_ex = impl_.get_executor();
          detail::get_connection_fill_adapter<std::decay_t<decltype(handler)>,
                                              connector_type>
              adapter{std::move(handler), &conn, request};
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), adapter, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
