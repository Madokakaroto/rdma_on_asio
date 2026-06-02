#pragma once

#include <span>
#include <utility>

#include "asio/io_context.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "nd/nd_connector.hpp"
#include "nd/detail/nd_listener_service.hpp"

namespace asio::rdma {

// Control-plane listener over NetworkDirect. Mirrors ibv_listener / asio's
// acceptor:
//   - open(port_space) / bind(endpoint) / listen(backlog)
//   - async_get_connection()        -> a new connector (peer connection)
//   - async_get_connection(conn)    -> fill a pre-built connector
template <typename PortSpace>
class nd_listener {
public:
  using service_type = detail::nd_listener_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using connector_type = nd_connector<PortSpace>;
  using native_connector_type = detail::nd_connector_handle_t;

  explicit nd_listener(asio::io_context& io_ctx) : impl_(0, 0, io_ctx) {
  }

  ~nd_listener() = default;
  nd_listener(nd_listener&&) = default;
  nd_listener& operator=(nd_listener&&) = default;
  nd_listener(nd_listener const&) = delete;
  nd_listener& operator=(nd_listener const&) = delete;

  void open(PortSpace const& ps, nd_config_t const& config = {}) {
    asio::error_code ec;
    open(ps, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(PortSpace const& ps, nd_config_t const& config,
            asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), ps, config, ec);
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
  // the listener's io_context with the adopted IND2Connector + client's pd.
  template <typename AcceptToken>
  auto async_get_connection(AcceptToken&& token) {
    return asio::async_initiate<AcceptToken,
                                void(asio::error_code, connector_type)>(
        [this](auto handler) {
          auto io_ex = impl_.get_executor();
          auto& io_ctx = io_ex.context();
          // Bind the wrapper to a named local — the service takes Handler& and
          // moves it; a temporary won't bind (the same bug fixed on ibv).
          auto wrapper = [&io_ctx, h = std::move(handler)](
                             asio::error_code ec, native_connector_type handle,
                             std::span<const std::byte> pd) mutable {
            connector_type conn(io_ctx);
            if (!ec) {
              asio::error_code aec;
              conn.assign_with_private_data(std::move(handle), pd,
                                            nd_config_t{}, aec);
              if (aec) ec = aec;
            }
            std::move(h)(ec, std::move(conn));
          };
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), wrapper, io_ex);
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
          auto wrapper = [&conn, h = std::move(handler)](
                             asio::error_code ec, native_connector_type handle,
                             std::span<const std::byte> pd) mutable {
            if (!ec) {
              asio::error_code aec;
              conn.assign_with_private_data(std::move(handle), pd,
                                            nd_config_t{}, aec);
              if (aec) ec = aec;
            }
            std::move(h)(ec);
          };
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), wrapper, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
