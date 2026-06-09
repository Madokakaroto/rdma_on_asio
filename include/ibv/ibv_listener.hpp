#pragma once

#include <span>
#include <type_traits>
#include <utility>

#include "asio/associator.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_connector.hpp"
#include "ibv/detail/ibv_listener_service.hpp"

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

  void operator()(asio::error_code ec, ibv_connector_handle_t handle,
                  std::span<const std::byte> pd) {
    ConnectorType conn(*io_ctx_);
    if (!ec) {
      asio::error_code aec;
      conn.assign_with_private_data(std::move(handle), pd, aec);
      if (aec) ec = aec;
    }
    std::move(handler_)(ec, std::move(conn));
  }
};

template <typename Handler, typename ConnectorType>
struct get_connection_fill_adapter {
  Handler handler_;
  ConnectorType* conn_;

  void operator()(asio::error_code ec, ibv_connector_handle_t handle,
                  std::span<const std::byte> pd) {
    if (!ec) {
      asio::error_code aec;
      conn_->assign_with_private_data(std::move(handle), pd, aec);
      if (aec) ec = aec;
    }
    std::move(handler_)(ec);
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
//   - open(port_space) / bind(endpoint) / listen(backlog)
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
    impl_.get_service().open(impl_.get_implementation(), ps.rdma_type(), ec);
  }

  void bind(endpoint_type const& endpoint) {
    asio::error_code ec;
    bind(endpoint, ec);
    asio::detail::throw_error(ec);
  }

  void bind(endpoint_type const& endpoint, asio::error_code& ec) {
    impl_.get_service().bind(impl_.get_implementation(), endpoint, ec);
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

  // Return form: handler(error_code, connector_type). The connector is built on
  // the listener's io_context with the adopted cm_id + client's private data.
  template <typename AcceptToken>
  auto async_get_connection(AcceptToken&& token) {
    return asio::async_initiate<AcceptToken,
                                void(asio::error_code, connector_type)>(
        [this](auto handler) {
          auto io_ex = impl_.get_executor();
          detail::get_connection_adapter<std::decay_t<decltype(handler)>,
                                         connector_type>
              adapter{std::move(handler), &io_ex.context()};
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), adapter, io_ex);
        },
        token);
  }

  // Fill form: handler(error_code); assigns the connection into a pre-built
  // (empty) connector — lets the caller pick its io_context.
  template <typename AcceptToken>
  auto async_get_connection(connector_type& conn, AcceptToken&& token) {
    return asio::async_initiate<AcceptToken, void(asio::error_code)>(
        [this, &conn](auto handler) {
          auto io_ex = impl_.get_executor();
          detail::get_connection_fill_adapter<std::decay_t<decltype(handler)>,
                                              connector_type>
              adapter{std::move(handler), &conn};
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), adapter, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
