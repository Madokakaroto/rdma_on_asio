#pragma once

#include <span>
#include "asio/io_context.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "nd/detail/nd_connector_service.hpp"
#include "nd/nd_queue_pair.hpp"

namespace asio::rdma {

template <typename PortSpace>
class nd_connector {
public:
  using service_type = detail::nd_connector_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = detail::nd_connector_handle_t;

  explicit nd_connector(asio::io_context& io_ctx)
      : impl_(0, 0, io_ctx) {
  }

  ~nd_connector() = default;
  nd_connector(nd_connector&&) = default;
  nd_connector& operator=(nd_connector&&) = default;
  nd_connector(nd_connector const&) = delete;
  nd_connector& operator=(nd_connector const&) = delete;

  // client open
  void open(nd_queue_pair<PortSpace>& qp, nd_config_t const& config = {}) {
    asio::error_code ec;
    open(qp, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(nd_queue_pair<PortSpace>& qp, nd_config_t const& config,
            asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(),
                             qp.native_handle(), config, ec);
  }

  // server open (from listener's native connector)
  void open(native_connector_type&& connector,
            nd_queue_pair<PortSpace>& qp,
            nd_config_t const& config = {}) {
    asio::error_code ec;
    open(std::move(connector), qp, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(native_connector_type&& connector,
            nd_queue_pair<PortSpace>& qp,
            nd_config_t const& config, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(),
                             std::move(connector), qp.native_handle(),
                             config, ec);
  }

  bool is_open() const noexcept {
    return impl_.get_service().is_open(impl_.get_implementation());
  }

  void cancel() {
    impl_.get_service().cancel(impl_.get_implementation());
  }

  // async connect: handler(error_code)
  template <typename ConnectToken>
  auto async_connect(endpoint_type const& endpoint,
                     std::span<const std::byte> outgoing_private_data,
                     ConnectToken&& token) {
    return asio::async_initiate<ConnectToken, void(asio::error_code)>(
        [this, &endpoint, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_connect(
              impl_.get_implementation(), endpoint,
              outgoing_private_data, handler, io_ex);
        },
        token);
  }

  // async accept: handler(error_code)
  template <typename AcceptToken>
  auto async_accept(std::span<const std::byte> outgoing_private_data,
                    AcceptToken&& token) {
    return asio::async_initiate<AcceptToken, void(asio::error_code)>(
        [this, outgoing_private_data](auto handler) {
          auto io_ex = impl_.get_executor();
          impl_.get_service().async_accept(
              impl_.get_implementation(), outgoing_private_data,
              handler, io_ex);
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

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
