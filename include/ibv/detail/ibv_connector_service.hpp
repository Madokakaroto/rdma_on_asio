#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "asio/buffer.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/execution_context.hpp"
#include "asio/io_context.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_op_accept.hpp"
#include "ibv/detail/ibv_op_connect.hpp"
#include "ibv/detail/ibv_ops_cm.hpp"
#include "ibv/detail/ibv_service_base.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"

namespace asio::rdma::detail {

// Control-plane service for ibv_connector: owns a cm_id + its event channel,
// registered to the epoll reactor. Drives connect / accept / disconnect via
// reactor_op state machines. Mirrors nd_connector_service (IOCP -> epoll).
//
// The QP is created on the cm_id during connect/accept using a create-qp
// callback supplied by the queue_pair passed to async_connect/async_accept.
template <typename PortSpace>
class ibv_connector_service
    : public asio::detail::execution_context_service_base<
          ibv_connector_service<PortSpace>>
    , public ibv_service_base {
public:
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = ibv_connector_handle_t;

  struct implementation_type : ibv_service_base::base_implementation_type {
    cm_channel_holder cm_channel_;
    cm_id_holder cm_id_;
    asio::detail::reactor::per_descriptor_data cm_reactor_data_;
    ibv_config_t config_;
    int timeout_ = default_cm_timeout_ms;
    // Peer's private data (client request on server; server reply on client).
    std::array<std::byte, max_private_data_size> private_data_buffer_{};
    std::size_t private_data_length_ = 0;
  };

  explicit ibv_connector_service(asio::execution_context& context)
      : asio::detail::execution_context_service_base<
            ibv_connector_service<PortSpace>>(context)
      , ibv_service_base(context) {
  }

  void shutdown() {
    base_shutdown<implementation_type>(
        [this](implementation_type& impl) { close_for_destruction(impl); });
  }

  void construct(implementation_type& impl) {
    base_construct(impl);
    impl.cm_reactor_data_ = asio::detail::reactor::per_descriptor_data();
    impl.timeout_ = default_cm_timeout_ms;
  }

  void destroy(implementation_type& impl) {
    close_for_destruction(impl);
    base_destroy(impl);
  }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    base_move_construct(impl, other_impl);
    impl.cm_channel_ = std::move(other_impl.cm_channel_);
    impl.cm_id_ = std::move(other_impl.cm_id_);
    impl.config_ = other_impl.config_;
    impl.timeout_ = other_impl.timeout_;
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    impl.cm_reactor_data_ = asio::detail::reactor::per_descriptor_data();
    if (impl.cm_channel_) {
      this->reactor_.move_descriptor(impl.cm_channel_->fd, impl.cm_reactor_data_,
                                     other_impl.cm_reactor_data_);
    }
  }

  void move_assign(implementation_type& impl,
                   ibv_connector_service& /*other_service*/,
                   implementation_type& other_impl) {
    close_for_destruction(impl);
    impl.cm_channel_ = std::move(other_impl.cm_channel_);
    impl.cm_id_ = std::move(other_impl.cm_id_);
    impl.config_ = other_impl.config_;
    impl.timeout_ = other_impl.timeout_;
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    if (impl.cm_channel_) {
      this->reactor_.move_descriptor(impl.cm_channel_->fd, impl.cm_reactor_data_,
                                     other_impl.cm_reactor_data_);
    }
  }

  // --- open / assign (asio basic_socket::open(protocol) / assign) ---

  // Create a fresh event channel + cm_id (client). ps_type = PortSpace::rdma_type().
  void open(implementation_type& impl, rdma_port_space ps_type,
            ibv_config_t const& config, asio::error_code& ec) {
    if (is_open(impl)) {
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    cm_channel_holder channel{ create_event_channel(ec) };
    if (ec) {
      return;
    }
    native_cm_id_t* id = nullptr;
    if (create_cm_id(channel.get(), &id, nullptr, ps_type, ec) != 0) {
      return;
    }
    cm_id_holder cm_id{ id };
    if (int err = this->reactor_.register_descriptor(channel->fd,
                                                     impl.cm_reactor_data_)) {
      ec = make_system_error_code(err);
      return;
    }
    impl.cm_channel_ = std::move(channel);
    impl.cm_id_ = std::move(cm_id);
    impl.config_ = config;
    impl.timeout_ = default_cm_timeout_ms;
    ec.clear();
  }

  // Adopt a connector handle produced by the listener (server). Also store the
  // client's request private data so get_remote_data() can return it.
  void assign(implementation_type& impl, native_connector_type&& handle,
              std::span<const std::byte> remote_pd, ibv_config_t const& config,
              asio::error_code& ec) {
    if (is_open(impl)) {
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    if (!handle.cm_channel_ || !handle.cm_id_) {
      ec = make_error_code(ibv_errc::ext_invalid_device);
      return;
    }
    if (int err = this->reactor_.register_descriptor(handle.cm_channel_->fd,
                                                     impl.cm_reactor_data_)) {
      ec = make_system_error_code(err);
      return;
    }
    impl.cm_channel_ = std::move(handle.cm_channel_);
    impl.cm_id_ = std::move(handle.cm_id_);
    impl.config_ = config;
    impl.timeout_ = default_cm_timeout_ms;
    store_remote_pd(impl, remote_pd);
    ec.clear();
  }

  bool is_open(implementation_type const& impl) const {
    return impl.cm_id_ != nullptr;
  }

  asio::const_buffer get_remote_data(implementation_type const& impl) const {
    return asio::buffer(impl.private_data_buffer_.data(),
                        impl.private_data_length_);
  }

  void cancel(implementation_type& impl) {
    if (impl.cm_channel_) {
      this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
    }
  }

  // --- async operations ---

  template <typename Handler, typename IoExecutor>
  void async_connect(implementation_type& impl, ibv_create_qp_fn create_qp,
                     endpoint_type const& endpoint,
                     asio::const_buffer private_data, Handler& handler,
                     IoExecutor const& io_ex) {
    // Auto-open (asio socket.connect opens with the endpoint's protocol).
    asio::error_code open_ec;
    if (!is_open(impl)) {
      open(impl, PortSpace::rdma_type(), ibv_config_t{}, open_ec);
    }
    using op = ibv_connect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{this->success_ec_, impl.cm_id_.get(), impl.timeout_,
                       private_data.data(), private_data.size(),
                       std::move(create_qp), pd_sink(impl), handler, io_ex};
    if (open_ec) {
      p.p->ec_ = open_ec;
      this->reactor_.post_immediate_completion(p.p, false);
    }
    else {
      start_connect_op(impl, endpoint, p.p);
    }
    p.v = p.p = 0;
  }

  template <typename Handler, typename IoExecutor>
  void async_accept(implementation_type& impl, ibv_create_qp_fn create_qp,
                    asio::const_buffer private_data, Handler& handler,
                    IoExecutor const& io_ex) {
    using op = ibv_accept_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{this->success_ec_, impl.cm_id_.get(), handler, io_ex};
    start_accept_op(impl, std::move(create_qp), private_data, p.p);
    p.v = p.p = 0;
  }

  template <typename Handler, typename IoExecutor>
  void async_disconnect(implementation_type& impl, Handler& handler,
                        IoExecutor const& io_ex) {
    using op = ibv_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{this->success_ec_, impl.cm_id_.get(), handler, io_ex};
    start_disconnect_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  ibv_pd_sink pd_sink(implementation_type& impl) {
    return {impl.private_data_buffer_.data(), impl.private_data_buffer_.size(), &impl.private_data_length_};
  }

  void store_remote_pd(implementation_type& impl,
                       std::span<const std::byte> pd) {
    impl.private_data_length_ = (std::min)(pd.size(), impl.private_data_buffer_.size());
    if (impl.private_data_length_) {
      std::memcpy(impl.private_data_buffer_.data(), pd.data(), impl.private_data_length_);
    }
  }

  void start_connect_op(implementation_type& impl,
                        endpoint_type const& endpoint,
                        asio::detail::reactor_op* op) {
    if (resolve_addr(impl.cm_id_.get(), nullptr,
                     const_cast<sockaddr*>(endpoint.data()), impl.timeout_,
                     op->ec_) == 0) {
      op->ec_ = asio::error_code{};
      this->reactor_.start_op(asio::detail::reactor::read_op,
                              impl.cm_channel_->fd, impl.cm_reactor_data_, op,
                              false, false);
    }
    else {
      this->reactor_.post_immediate_completion(op, false);
    }
  }

  void start_accept_op(implementation_type& impl, ibv_create_qp_fn create_qp,
                       asio::const_buffer private_data,
                       asio::detail::reactor_op* op) {
    // The child cm_id already has a context: create the QP before accepting.
    if (create_qp) {
      op->ec_ = create_qp(impl.cm_id_.get());
      if (op->ec_) {
        this->reactor_.post_immediate_completion(op, false);
        return;
      }
    }
    rdma_conn_param param{};
    param.private_data = private_data.data();
    param.private_data_len = static_cast<std::uint8_t>(private_data.size());
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.rnr_retry_count = 7;
    if (accept(impl.cm_id_.get(), &param, op->ec_) == 0) {
      op->ec_ = asio::error_code{};
      this->reactor_.start_op(asio::detail::reactor::read_op,
                              impl.cm_channel_->fd, impl.cm_reactor_data_, op,
                              false, false);
    }
    else {
      this->reactor_.post_immediate_completion(op, false);
    }
  }

  void start_disconnect_op(implementation_type& impl,
                           asio::detail::reactor_op* op) {
    // rdma_disconnect synchronously transitions the local QP to error and
    // flushes pending WRs; the active disconnector does not reliably receive a
    // DISCONNECTED CM event, so complete immediately rather than waiting.
    disconnect(impl.cm_id_.get(), op->ec_);
    this->reactor_.post_immediate_completion(op, false);
  }

  void close_for_destruction(implementation_type& impl) {
    if (impl.cm_channel_) {
      this->reactor_.deregister_descriptor(impl.cm_channel_->fd,
                                           impl.cm_reactor_data_, false);
      this->reactor_.cleanup_descriptor_data(impl.cm_reactor_data_);
    }
    // The QP is created on cm_id (cm_id->qp) and owned here; destroy it before
    // the cm_id, which in turn must precede the event channel.
    if (impl.cm_id_ && impl.cm_id_->qp) {
      ::rdma_destroy_qp(impl.cm_id_.get());
    }
    impl.cm_id_.reset();
    impl.cm_channel_.reset();
  }
};

}
