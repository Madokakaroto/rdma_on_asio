#pragma once

#include "asio/detail/config.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "asio/ip/address.hpp"
#include "nd/detail/nd_service_base.hpp"
#include "nd/detail/nd_io_completion_service.hpp"
#include "nd/detail/nd_ops_cm.hpp"
#include "nd/detail/nd_op_connect.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma::detail {

struct nd_connector_handle_t {
  nd2_connector_ptr connector_;
  unique_handle_t overlapped_handle_;
  nd_adapter_ptr adapter_;

  nd_connector_handle_t() = default;
  nd_connector_handle_t(nd_connector_handle_t&&) = default;
  nd_connector_handle_t& operator=(nd_connector_handle_t&&) = default;
  nd_connector_handle_t(nd_connector_handle_t const&) = delete;
  nd_connector_handle_t& operator=(nd_connector_handle_t const&) = delete;
};

template <typename PortSpace>
class nd_connector_service
    : public asio::detail::execution_context_service_base<
          nd_connector_service<PortSpace>>
    , public nd_service_base {
public:
  using base_type = asio::detail::execution_context_service_base<
      nd_connector_service<PortSpace>>;
  using endpoint_type = typename PortSpace::endpoint;

  struct implementation_type : nd_service_base::base_implementation_type {
    nd2_connector_ptr connector_;
    unique_handle_t connector_handle_;
    native_qp_t* qp_ = nullptr;
    nd_adapter_ptr adapter_;
    nd_config_t config_;
  };

  explicit nd_connector_service(asio::execution_context& ctx)
      : base_type(ctx)
      , nd_service_base(ctx) {
  }

  ~nd_connector_service() = default;

  void shutdown() override {
    base_shutdown<implementation_type>([](implementation_type& impl) {
      impl.connector_.Reset();
      impl.connector_handle_.reset();
      impl.qp_ = nullptr;
      impl.adapter_.reset();
    });
  }

  // lifecycle
  void construct(implementation_type& impl) {
    nd_service_base::base_construct(impl);
    impl.connector_.Reset();
    impl.connector_handle_.reset();
    impl.qp_ = nullptr;
    impl.adapter_.reset();
  }

  void destroy(implementation_type& impl) {
    impl.connector_.Reset();
    impl.connector_handle_.reset();
    impl.qp_ = nullptr;
    impl.adapter_.reset();
    nd_service_base::base_destroy(impl);
  }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    nd_service_base::base_move_construct(impl, other_impl);
    impl.connector_ = std::move(other_impl.connector_);
    impl.connector_handle_ = std::move(other_impl.connector_handle_);
    impl.qp_ = other_impl.qp_;
    impl.adapter_ = std::move(other_impl.adapter_);
    impl.config_ = other_impl.config_;
    other_impl.qp_ = nullptr;
  }

  void move_assign(implementation_type& impl,
                   nd_connector_service& other_service,
                   implementation_type& other_impl) {
    impl.connector_.Reset();
    impl.connector_handle_.reset();
    nd_service_base::base_destroy(impl);
    nd_service_base::base_construct(impl);
    impl.connector_ = std::move(other_impl.connector_);
    impl.connector_handle_ = std::move(other_impl.connector_handle_);
    impl.qp_ = other_impl.qp_;
    impl.adapter_ = std::move(other_impl.adapter_);
    impl.config_ = other_impl.config_;
    other_impl.qp_ = nullptr;
  }

  // open (client: create new connector)
  void open(implementation_type& impl, native_qp_t* qp,
            nd_config_t const& config, asio::error_code& ec) {
    if (impl.connector_) {
      ec = asio::error::already_open;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    if (!qp) {
      ec = nd_errc::ext_invalid_connector;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    auto& io_svc =
        asio::use_service<nd_io_completion_service>(this->context());
    if (!io_svc.is_initialized()) {
      ec = nd_errc::ext_invalid_device;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    auto adapter = io_svc.get_adapter();
    impl.connector_handle_.reset(
        create_overlapped_file(adapter->adapter_.Get(), ec));
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.connector_.Attach(
        create_connector(adapter->adapter_.Get(),
                         impl.connector_handle_.get(), ec));
    if (ec) {
      impl.connector_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    this->scheduler_.register_handle(impl.connector_handle_.get(), ec);
    if (ec) {
      impl.connector_.Reset();
      impl.connector_handle_.reset();
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.qp_ = qp;
    impl.adapter_ = adapter;
    impl.config_ = config;
  }

  // open (server: from native connector handle)
  void open(implementation_type& impl, nd_connector_handle_t&& handle,
            native_qp_t* qp, nd_config_t const& config,
            asio::error_code& ec) {
    if (impl.connector_) {
      ec = asio::error::already_open;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    if (!qp || !handle.connector_) {
      ec = nd_errc::ext_invalid_connector;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    this->scheduler_.register_handle(handle.overlapped_handle_.get(), ec);
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.connector_ = std::move(handle.connector_);
    impl.connector_handle_ = std::move(handle.overlapped_handle_);
    impl.adapter_ = std::move(handle.adapter_);
    impl.qp_ = qp;
    impl.config_ = config;
  }

  bool is_open(implementation_type const& impl) const noexcept {
    return impl.connector_ != nullptr;
  }

  void cancel(implementation_type& impl) {
    // TODO: cancel in-flight operations
  }

  // async connect
  template <typename Handler, typename IoExecutor>
  void async_connect(implementation_type& impl, endpoint_type const& endpoint,
                     std::span<const std::byte> private_data,
                     Handler& handler, IoExecutor const& io_ex) {
    using op = nd_connect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), handler, io_ex};
    start_connect_op(impl, endpoint, private_data, p.p);
    p.v = p.p = 0;
  }

  // async accept
  template <typename Handler, typename IoExecutor>
  void async_accept(implementation_type& impl,
                    std::span<const std::byte> private_data,
                    Handler& handler, IoExecutor const& io_ex) {
    using op = nd_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), handler, io_ex};
    start_accept_op(impl, private_data, p.p);
    p.v = p.p = 0;
  }

  // async disconnect
  template <typename Handler, typename IoExecutor>
  void async_disconnect(implementation_type& impl,
                        Handler& handler, IoExecutor const& io_ex) {
    using op = nd_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), handler, io_ex};
    start_disconnect_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  void start_connect_op(implementation_type& impl,
                        endpoint_type const& endpoint,
                        std::span<const std::byte> private_data,
                        nd_connect_op_base* op) {
    this->scheduler_.work_started();

    endpoint_type local_ep{asio::ip::make_address(impl.adapter_->name_),
                           endpoint.port()};

    asio::error_code ec{};
    bind_addr(impl.connector_.Get(), local_ep.data(), local_ep.size(), ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }

    connect(impl.connector_.Get(), impl.qp_,
            endpoint.data(), endpoint.size(),
            impl.config_.inbound_read_limit_,
            impl.config_.outbound_read_limit_,
            private_data.empty() ? nullptr : private_data.data(),
            static_cast<ULONG>(private_data.size()),
            op, ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }
    this->scheduler_.on_pending(op);
  }

  void start_accept_op(implementation_type& impl,
                       std::span<const std::byte> private_data,
                       nd_op_base* op) {
    this->scheduler_.work_started();
    asio::error_code ec{};
    accept(impl.connector_.Get(), impl.qp_,
           impl.config_.inbound_read_limit_,
           impl.config_.outbound_read_limit_,
           private_data.empty() ? nullptr : private_data.data(),
           static_cast<ULONG>(private_data.size()),
           op, ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }
    this->scheduler_.on_pending(op);
  }

  void start_disconnect_op(implementation_type& impl, nd_op_base* op) {
    this->scheduler_.work_started();
    asio::error_code ec{};
    disconnect(impl.connector_.Get(), op, ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }
    this->scheduler_.on_pending(op);
  }
};

}
