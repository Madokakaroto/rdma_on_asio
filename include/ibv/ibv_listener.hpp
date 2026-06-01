#pragma once

#include <span>

#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/detail/ibv_listener_service.hpp"

namespace asio::rdma {

// Control-plane listener over rdma_cm. Mirrors nd_listener; bind takes an
// endpoint (rdma_cm binds by sockaddr, like asio's acceptor).
template <typename PortSpace>
class ibv_listener {
public:
  using service_type = detail::ibv_listener_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = detail::ibv_connector_handle_t;

  explicit ibv_listener(asio::io_context& io_ctx) : impl_(0, 0, io_ctx) {
  }

  ~ibv_listener() = default;
  ibv_listener(ibv_listener&&) = default;
  ibv_listener& operator=(ibv_listener&&) = default;
  ibv_listener(ibv_listener const&) = delete;
  ibv_listener& operator=(ibv_listener const&) = delete;

  void open(ibv_config_t const& config = {}) {
    asio::error_code ec;
    open(config, ec);
    asio::detail::throw_error(ec);
  }

  void open(ibv_config_t const& config, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), config, ec);
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

  // handler signature: void(error_code, native_connector_type, span<const byte>)
  template <typename AcceptToken>
  auto async_get_connection_request(AcceptToken&& token) {
    return asio::async_initiate<
        AcceptToken, void(asio::error_code, native_connector_type,
                          std::span<const std::byte>)>(
        [this](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_get_connection_request(
              impl_.get_implementation(), handler, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
