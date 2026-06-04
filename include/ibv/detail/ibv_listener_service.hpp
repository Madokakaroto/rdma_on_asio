#pragma once

#include "asio/detail/reactor.hpp"
#include "asio/execution_context.hpp"
#include "asio/io_context.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_io_completion_service.hpp"
#include "ibv/detail/ibv_op_get_connection_request.hpp"
#include "ibv/detail/ibv_ops_cm.hpp"
#include "ibv/detail/ibv_service_base.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"

namespace asio::rdma::detail {

// Control-plane service for ibv_listener: owns a listening cm_id + its event
// channel, registered to the epoll reactor. Mirrors nd_listener_service.
template <typename PortSpace>
class ibv_listener_service
    : public asio::detail::execution_context_service_base<
          ibv_listener_service<PortSpace>>
    , public ibv_service_base {
public:
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = ibv_connector_handle_t;

  struct implementation_type : ibv_service_base::base_implementation_type {
    cm_channel_holder cm_channel_;
    cm_id_holder cm_id_;
    asio::detail::reactor::per_descriptor_data cm_reactor_data_;
  };

  explicit ibv_listener_service(asio::execution_context& context)
      : asio::detail::execution_context_service_base<
            ibv_listener_service<PortSpace>>(context)
      , ibv_service_base(context) {
  }

  void shutdown() {
    base_shutdown<implementation_type>(
        [this](implementation_type& impl) { close_for_destruction(impl); });
  }

  void construct(implementation_type& impl) {
    base_construct(impl);
    impl.cm_reactor_data_ = asio::detail::reactor::per_descriptor_data();
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
    impl.cm_reactor_data_ = asio::detail::reactor::per_descriptor_data();
    if (impl.cm_channel_) {
      this->reactor_.move_descriptor(impl.cm_channel_->fd, impl.cm_reactor_data_,
                                     other_impl.cm_reactor_data_);
    }
  }

  void move_assign(implementation_type& impl,
                   ibv_listener_service& /*other_service*/,
                   implementation_type& other_impl) {
    close_for_destruction(impl);
    impl.cm_channel_ = std::move(other_impl.cm_channel_);
    impl.cm_id_ = std::move(other_impl.cm_id_);
    if (impl.cm_channel_) {
      this->reactor_.move_descriptor(impl.cm_channel_->fd, impl.cm_reactor_data_,
                                     other_impl.cm_reactor_data_);
    }
  }

  // --- open / bind / listen ---

  void open(implementation_type& impl, rdma_port_space ps_type,
            asio::error_code& ec) {
    if (is_open(impl)) {
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    if (!asio::use_service<ibv_io_completion_service>(this->context())
             .is_initialized()) {
      ec = make_error_code(ibv_errc::ext_device_not_registered);
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
    // Register the local block's fd (not the not-yet-moved impl field — that was
    // the deprecated bug).
    if (int err = this->reactor_.register_descriptor(channel->fd,
                                                     impl.cm_reactor_data_)) {
      ec = make_system_error_code(err);
      return;
    }
    impl.cm_channel_ = std::move(channel);
    impl.cm_id_ = std::move(cm_id);
    ec.clear();
  }

  void bind(implementation_type& impl, endpoint_type const& endpoint,
            asio::error_code& ec) {
    bind_addr(impl.cm_id_.get(), endpoint.data(), ec);
  }

  void listen(implementation_type& impl, int backlog, asio::error_code& ec) {
    detail::listen(impl.cm_id_.get(), backlog, ec);
  }

  bool is_open(implementation_type const& impl) const {
    return impl.cm_id_ != nullptr;
  }

  void cancel(implementation_type& impl) {
    if (impl.cm_channel_) {
      this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
    }
  }

  // handler(error_code, ibv_connector_handle_t, std::span<const std::byte>)
  template <typename Handler, typename IoExecutor>
  void async_get_connection_request(implementation_type& impl, Handler& handler,
                                    IoExecutor const& io_ex) {
    using op = ibv_get_connection_request_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v)
        op{this->success_ec_, impl.cm_channel_.get(), handler, io_ex};
    start_get_connection_request_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  void start_get_connection_request_op(implementation_type& impl,
                                       asio::detail::reactor_op* op) {
    if (!is_open(impl)) {
      op->ec_ = asio::error::bad_descriptor;
      this->reactor_.post_immediate_completion(op, false);
      return;
    }
    // No CM call to initiate: CONNECT_REQUEST events flow on the listener's
    // channel as clients connect. Just arm the op on that fd.
    this->reactor_.start_op(asio::detail::reactor::read_op,
                            impl.cm_channel_->fd, impl.cm_reactor_data_, op,
                            false, false);
  }

  void close_for_destruction(implementation_type& impl) {
    if (impl.cm_channel_) {
      this->reactor_.deregister_descriptor(impl.cm_channel_->fd,
                                           impl.cm_reactor_data_, false);
      this->reactor_.cleanup_descriptor_data(impl.cm_reactor_data_);
    }
    impl.cm_id_.reset();
    impl.cm_channel_.reset();
  }
};

}
