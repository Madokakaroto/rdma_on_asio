#pragma once

#include <span>

#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_queue_pair.hpp"
#include "ibv/detail/ibv_connector_service.hpp"

namespace asio::rdma {

// Control-plane connector over rdma_cm. Mirrors nd_connector. The bound
// queue_pair's QP is created on this connector's cm_id during connect/accept.
template <typename PortSpace>
class ibv_connector {
public:
  using service_type = detail::ibv_connector_service<PortSpace>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = detail::ibv_connector_handle_t;

  explicit ibv_connector(asio::io_context& io_ctx) : impl_(0, 0, io_ctx) {
  }

  ~ibv_connector() = default;
  ibv_connector(ibv_connector&&) = default;
  ibv_connector& operator=(ibv_connector&&) = default;
  ibv_connector(ibv_connector const&) = delete;
  ibv_connector& operator=(ibv_connector const&) = delete;

  // client open: fresh cm_id + event channel; binds the qp so its QP is created
  // on this cm_id during connect.
  void open(ibv_queue_pair& qp, ibv_config_t const& config = {}) {
    asio::error_code ec;
    open(qp, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(ibv_queue_pair& qp, ibv_config_t const& config,
            asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(),
                             qp.make_create_qp_fn(), config, ec);
  }

  // server open: adopt a connector handle from the listener; bind the qp so its
  // QP is created on the adopted cm_id during accept.
  void open(native_connector_type&& connector, ibv_queue_pair& qp,
            ibv_config_t const& config = {}) {
    asio::error_code ec;
    open(std::move(connector), qp, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(native_connector_type&& connector, ibv_queue_pair& qp,
            ibv_config_t const& config, asio::error_code& ec) {
    impl_.get_service().open(impl_.get_implementation(), std::move(connector),
                             qp.make_create_qp_fn(), config, ec);
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
          impl_.get_service().async_connect(impl_.get_implementation(),
                                            endpoint, outgoing_private_data,
                                            handler, io_ex);
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
          impl_.get_service().async_accept(impl_.get_implementation(),
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
          impl_.get_service().async_disconnect(impl_.get_implementation(),
                                               handler, io_ex);
        },
        token);
  }

private:
  asio::detail::io_object_impl<service_type> impl_;
};

}
