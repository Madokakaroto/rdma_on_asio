#pragma once

#include "asio/associated_cancellation_slot.hpp"
#include "asio/detail/config.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "asio/ip/address.hpp"
#include "nd/detail/nd_service_base.hpp"
#include "nd/detail/nd_device_service.hpp"
#include "nd/detail/nd_ops_cm.hpp"
#include "nd/detail/nd_op_get_connection_request.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma::detail {

template <typename PortSpace>
class nd_listener_service
    : public asio::detail::execution_context_service_base<
          nd_listener_service<PortSpace>>
    , public nd_service_base {
public:
  using base_type = asio::detail::execution_context_service_base<
      nd_listener_service<PortSpace>>;
  using endpoint_type = typename PortSpace::endpoint;

  struct implementation_type : nd_service_base::base_implementation_type {
    nd2_listener_ptr listener_;
    unique_handle_t listener_handle_;
    nd_adapter_ptr adapter_;
  };

  explicit nd_listener_service(asio::execution_context& ctx)
      : base_type(ctx)
      , nd_service_base(ctx)
      , device_svc_(asio::use_service<nd_device_service>(ctx)) {
  }

  ~nd_listener_service() = default;

  void shutdown() override {
    base_shutdown<implementation_type>([](implementation_type& impl) {
      impl.listener_.Reset();
      impl.listener_handle_.reset();
      impl.adapter_.reset();
    });
  }

  // lifecycle
  void construct(implementation_type& impl) {
    nd_service_base::base_construct(impl);
    impl.listener_.Reset();
    impl.listener_handle_.reset();
    impl.adapter_.reset();
  }

  void destroy(implementation_type& impl) {
    impl.listener_.Reset();
    impl.listener_handle_.reset();
    impl.adapter_.reset();
    nd_service_base::base_destroy(impl);
  }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    nd_service_base::base_move_construct(impl, other_impl);
    impl.listener_ = std::move(other_impl.listener_);
    impl.listener_handle_ = std::move(other_impl.listener_handle_);
    impl.adapter_ = std::move(other_impl.adapter_);
  }

  void move_assign(implementation_type& impl,
                   nd_listener_service& other_service,
                   implementation_type& other_impl) {
    impl.listener_.Reset();
    impl.listener_handle_.reset();
    nd_service_base::base_destroy(impl);
    nd_service_base::base_construct(impl);
    impl.listener_ = std::move(other_impl.listener_);
    impl.listener_handle_ = std::move(other_impl.listener_handle_);
    impl.adapter_ = std::move(other_impl.adapter_);
  }

  // open: create listener + overlapped handle. PortSpace value is accepted for
  // parity with ibv (and possible future v4/v6 selection); ND has no explicit
  // RDMA port space. Requires use_device() on this io_context.
  void open(implementation_type& impl, PortSpace const& /*ps*/,
            asio::error_code& ec) {
    if (impl.listener_) {
      ec = asio::error::already_open;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    if (!device_svc_.is_registered()) {
      ec = nd_errc::ext_device_not_registered;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    auto adapter = device_svc_.get_device();
    impl.listener_handle_.reset(
        create_overlapped_file(adapter->adapter_.Get(), ec));
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.listener_.Attach(
        create_listener(adapter->adapter_.Get(),
                        impl.listener_handle_.get(), ec));
    if (ec) {
      impl.listener_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    this->scheduler_.register_handle(impl.listener_handle_.get(), ec);
    if (ec) {
      impl.listener_.Reset();
      impl.listener_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.adapter_ = adapter;
  }

  bool is_open(implementation_type const& impl) const noexcept {
    return impl.listener_ != nullptr;
  }

  void bind(implementation_type& impl, endpoint_type const& endpoint,
            asio::error_code& ec) {
    if (!is_open(impl)) {
      ec = nd_errc::ext_invalid_listener;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    bind_addr(impl.listener_.Get(), endpoint.data(), endpoint.size(), ec);
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
    }
  }

  void listen(implementation_type& impl, int backlog, asio::error_code& ec) {
    if (!is_open(impl)) {
      ec = nd_errc::ext_invalid_listener;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    detail::listen(impl.listener_.Get(), backlog, ec);
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
    }
  }

  void cancel(implementation_type& impl) {
    if (impl.listener_handle_)
      ::CancelIoEx(impl.listener_handle_.get(), NULL);
  }

  // async get_connection_request
  template <typename Handler, typename IoExecutor>
  void async_get_connection_request(implementation_type& impl,
                                    Handler& handler, IoExecutor const& io_ex) {
    auto cancel_slot = asio::get_associated_cancellation_slot(handler);
    using op = nd_get_connection_request_op<Handler, IoExecutor>;

    asio::error_code ec;
    nd_connector_handle_t connector_handle;
    connector_handle.adapter_ = impl.adapter_;
    connector_handle.overlapped_handle_.reset(
        create_overlapped_file(impl.adapter_->adapter_.Get(), ec));
    if (ec) {
      typename op::ptr p = {asio::detail::addressof(handler),
                            op::ptr::allocate(handler), 0};
      p.p = new (p.v) op{impl.listener_.Get(),
                          std::move(connector_handle), handler, io_ex};
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }

    connector_handle.connector_.Attach(
        create_connector(impl.adapter_->adapter_.Get(),
                         connector_handle.overlapped_handle_.get(), ec));
    if (ec) {
      typename op::ptr p = {asio::detail::addressof(handler),
                            op::ptr::allocate(handler), 0};
      p.p = new (p.v) op{impl.listener_.Get(),
                          std::move(connector_handle), handler, io_ex};
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }

    auto* connector_ptr = connector_handle.connector_.Get();
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.listener_.Get(),
                        std::move(connector_handle), handler, io_ex};

    this->scheduler_.work_started();
    get_connection_request(impl.listener_.Get(), connector_ptr, p.p, ec);
    if (ec) {
      this->scheduler_.on_completion(p.p, ec);
    } else {
      arm_nd_cancellation(cancel_slot, impl.listener_handle_.get(), p.p);
      this->scheduler_.on_pending(p.p);
    }
    p.v = p.p = 0;
  }

  nd_device_service& device_svc_;  // cached (registration guard + device)
};

}
