#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "asio/buffer.hpp"
#include "asio/detail/config.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "asio/ip/address.hpp"
#include "nd/detail/nd_service_base.hpp"
#include "nd/detail/nd_device_service.hpp"
#include "nd/detail/nd_ops_cm.hpp"
#include "nd/detail/nd_op_connect.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma::detail {

// Control-plane service for nd_connector. Mirrors ibv_connector_service.
//
// Open creates the IND2Connector + overlapped handle (no QP).
// The QP is supplied at async_connect/async_accept time and only borrowed --
// the queue_pair owns it.
template <typename PortSpace>
class nd_connector_service
    : public asio::detail::execution_context_service_base<
          nd_connector_service<PortSpace>>
    , public nd_service_base {
public:
  using base_type = asio::detail::execution_context_service_base<
      nd_connector_service<PortSpace>>;
  using endpoint_type = typename PortSpace::endpoint;
  using native_connector_type = nd_connector_handle_t;

  struct implementation_type : nd_service_base::base_implementation_type {
    nd2_connector_ptr connector_;
    unique_handle_t connector_handle_;
    nd_adapter_ptr adapter_;
    // Peer's private data (client request on server; server reply on client).
    std::array<std::byte, max_private_data_size> private_data_buffer_{};
    std::size_t private_data_length_ = 0;
    // Teardown/lifecycle, used for the discarded-state determination (aligned
    // with ibv's connect_state_): idle -> connecting -> [connected] -> closed.
    // nd-specific (no resolve stages). Plain (non-atomic): nd's disconnect is
    // not the MT-safe CAS arbiter ibv's is -- only touched on the initiating
    // side. A `!= idle` reuse guard makes the connector one-shot (rdma_cm-aligned).
    connect_state connect_state_ = connect_state::idle;
    // Peer-disconnect notification latch (set by self disconnect() or
    // NotifyDisconnect). SEPARATE from connect_state_ (mirrors ibv's peer_closed_):
    // makes async_wait_disconnect level-triggered. Kept as the wait-op's flag.
    bool disconnected_ = false;
  };

  explicit nd_connector_service(asio::execution_context& ctx)
      : base_type(ctx)
      , nd_service_base(ctx)
      , device_svc_(asio::use_service<nd_device_service>(ctx)) {
  }

  ~nd_connector_service() = default;

  void shutdown() override {
    base_shutdown<implementation_type>([](implementation_type& impl) {
      impl.connector_.Reset();
      impl.connector_handle_.reset();
      impl.adapter_.reset();
    });
  }

  // lifecycle
  void construct(implementation_type& impl) {
    nd_service_base::base_construct(impl);
    impl.connector_.Reset();
    impl.connector_handle_.reset();
    impl.adapter_.reset();
    impl.connect_state_ = connect_state::idle;
    impl.disconnected_ = false;
  }

  void destroy(implementation_type& impl) {
    impl.connector_.Reset();
    impl.connector_handle_.reset();
    impl.adapter_.reset();
    nd_service_base::base_destroy(impl);
  }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    nd_service_base::base_move_construct(impl, other_impl);
    impl.connector_ = std::move(other_impl.connector_);
    impl.connector_handle_ = std::move(other_impl.connector_handle_);
    impl.adapter_ = std::move(other_impl.adapter_);
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    impl.connect_state_ = other_impl.connect_state_;
    impl.disconnected_ = other_impl.disconnected_;
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
    impl.adapter_ = std::move(other_impl.adapter_);
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    impl.connect_state_ = other_impl.connect_state_;
    impl.disconnected_ = other_impl.disconnected_;
  }

  // open (client): create IND2Connector + overlapped handle. PortSpace value is
  // accepted for parity with ibv; nd has no rdma port space, so it's ignored.
  // Requires use_device() on this io_context (config is centralized there).
  void open(implementation_type& impl, PortSpace const& /*ps*/,
            asio::error_code& ec) {
    do_open(impl, ec);
  }

  // assign (server): adopt a connector handle from the listener and store the
  // client's request private data so remote_private_data() can return it.
  void assign(implementation_type& impl, native_connector_type&& handle,
              std::span<const std::byte> remote_pd, asio::error_code& ec) {
    if (impl.connector_) {
      ec = asio::error::already_open;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    if (!handle.connector_) {
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
    impl.connect_state_ = connect_state::idle;
    impl.disconnected_ = false;
    store_remote_pd(impl, remote_pd);
  }

  bool is_open(implementation_type const& impl) const noexcept {
    return impl.connector_ != nullptr;
  }

  asio::const_buffer get_remote_data(implementation_type const& impl) const {
    return asio::buffer(impl.private_data_buffer_.data(),
                        impl.private_data_length_);
  }

  void cancel(implementation_type& impl) {
    // TODO: cancel in-flight operations
  }

  // async connect: borrow qp from the queue_pair, auto-open if needed, then
  // Bind + Connect. CompleteConnect captures the server's reply private data.
  template <typename Handler, typename IoExecutor>
  void async_connect(implementation_type& impl, native_qp_t* qp,
                     endpoint_type const& endpoint,
                     asio::const_buffer private_data,
                     Handler& handler, IoExecutor const& io_ex) {
    // Auto-open (mirrors asio socket.connect opening with the protocol).
    asio::error_code open_ec;
    if (!is_open(impl)) {
      do_open(impl, open_ec);
    }

    using op = nd_connect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), pd_sink(impl), handler, io_ex};

    if (open_ec) {
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, open_ec);
      p.v = p.p = 0;
      return;
    }
    // Terminal / non-fresh connector: a prior connect attempt (one-shot) or a
    // disconnect()/failure left it non-idle. Early-exit with a clear code instead
    // of reusing a stranded IND2Connector; the user must create a fresh connector.
    // Mirrors the ibv `connect_state_ != idle` guard. [Windows verify]
    if (impl.connect_state_ != connect_state::idle) {
      asio::error_code ec = nd_errc::ext_connector_terminal;
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }
    if (!qp) {
      asio::error_code ec = nd_errc::ext_invalid_qp;
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }
    start_connect_op(impl, qp, endpoint, private_data, p.p);
    p.v = p.p = 0;
  }

  // async accept: borrow qp from the queue_pair, then Accept.
  template <typename Handler, typename IoExecutor>
  void async_accept(implementation_type& impl, native_qp_t* qp,
                    asio::const_buffer private_data,
                    Handler& handler, IoExecutor const& io_ex) {
    using op = nd_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), handler, io_ex};
    if (!device_registered()) {
      asio::error_code ec = nd_errc::ext_device_not_registered;
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }
    if (!qp) {
      asio::error_code ec = nd_errc::ext_invalid_qp;
      this->scheduler_.work_started();
      this->scheduler_.on_completion(p.p, ec);
      p.v = p.p = 0;
      return;
    }
    start_accept_op(impl, qp, private_data, p.p);
    p.v = p.p = 0;
  }

  // Synchronous, non-blocking, abrupt disconnect (mirrors socket shutdown/close).
  // ND2 Disconnect is overlapped, so fire-and-forget: issue it with a self-reaping
  // op (nd_disconnect_ff_op) and return without waiting. ec reflects only a
  // synchronous initiation failure (ND_PENDING is the normal async case -> not an
  // error). [Windows verify] the IND2Connector must outlive the in-flight Disconnect
  // overlapped (teardown ordering).
  void disconnect(implementation_type& impl, asio::error_code& ec) {
    if (!impl.connector_) {
      ec = asio::error::bad_descriptor;
      ASIO_ERROR_LOCATION(ec);
      return;
    }
    impl.connect_state_ = connect_state::closed;  // terminal (discarded)
    impl.disconnected_ = true;                     // peer/self latch (wait level-trigger)
    nd_disconnect_ff_op::Handler h{};
    nd_disconnect_ff_op::ptr p = {asio::detail::addressof(h),
                                  nd_disconnect_ff_op::ptr::allocate(h), 0};
    p.p = new (p.v) nd_disconnect_ff_op{impl.connector_.Get()};
    this->scheduler_.work_started();
    asio::error_code dec{};
    detail::disconnect(impl.connector_.Get(), p.p, dec);
    if (!dec || dec == nd_errc::pending) {
      this->scheduler_.on_pending(p.p);  // IOCP completes + frees it later
      ec.clear();
    }
    else {
      this->scheduler_.on_completion(p.p, dec);  // frees the op
      ec = dec;
    }
    p.v = p.p = 0;
  }

  // Disconnect NOTIFICATION (on_disconnect). One-shot; armed on demand via
  // IND2Connector::NotifyDisconnect. Level-triggered: if already disconnected,
  // completes immediately. Completion code: ext_disconnected. [Windows verify]
  template <typename Handler, typename IoExecutor>
  void async_wait_disconnect(implementation_type& impl, Handler& handler,
                             IoExecutor const& io_ex) {
    using op = nd_wait_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{impl.connector_.Get(), &impl.disconnected_, handler,
                       io_ex};
    this->scheduler_.work_started();
    if (impl.disconnected_) {
      // level-triggered: already disconnected -> complete immediately
      this->scheduler_.on_completion(p.p,
                                     make_error_code(nd_errc::ext_disconnected));
      p.v = p.p = 0;
      return;
    }
    if (!impl.connector_) {
      this->scheduler_.on_completion(
          p.p, make_error_code(nd_errc::ext_invalid_connector));
      p.v = p.p = 0;
      return;
    }
    asio::error_code ec{};
    detail::notify_disconnect(impl.connector_.Get(), p.p, ec);
    if (!ec || ec == nd_errc::pending) {
      this->scheduler_.on_pending(p.p);
    }
    else {
      this->scheduler_.on_completion(p.p, ec);
    }
    p.v = p.p = 0;
  }

private:
  // Worker for both public open(ps, ...) and auto-open inside async_connect.
  void do_open(implementation_type& impl, asio::error_code& ec) {
    if (impl.connector_) {
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

    impl.adapter_ = adapter;
  }

  // device_service holds the device + effective config (installed by use_device)
  // and answers the registration guard. Cached as a ref in the ctor.
  bool device_registered() { return device_svc_.is_registered(); }
  nd_config_t effective_config() {
    return device_svc_.is_registered() ? device_svc_.get_effective_config()
                                       : nd_config_t{};
  }

  nd_pd_sink pd_sink(implementation_type& impl) {
    return {impl.private_data_buffer_.data(), impl.private_data_buffer_.size(),
            &impl.private_data_length_};
  }

  void store_remote_pd(implementation_type& impl,
                       std::span<const std::byte> pd) {
    impl.private_data_length_ =
        (std::min)(pd.size(), impl.private_data_buffer_.size());
    if (impl.private_data_length_) {
      std::memcpy(impl.private_data_buffer_.data(), pd.data(),
                  impl.private_data_length_);
    }
  }

  void start_connect_op(implementation_type& impl, native_qp_t* qp,
                        endpoint_type const& endpoint,
                        asio::const_buffer private_data,
                        nd_connect_op_base* op) {
    this->scheduler_.work_started();
    // One-shot: mark non-idle on attempt so any reuse is rejected (rdma_cm-aligned).
    impl.connect_state_ = connect_state::connecting;

    endpoint_type local_ep{asio::ip::make_address(impl.adapter_->name_),
                           endpoint.port()};

    asio::error_code ec{};
    bind_addr(impl.connector_.Get(), local_ep.data(), local_ep.size(), ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }

    auto const eff = effective_config();
    connect(impl.connector_.Get(), qp,
            endpoint.data(), endpoint.size(),
            eff.inbound_read_limit_,
            eff.outbound_read_limit_,
            private_data.size() == 0 ? nullptr : private_data.data(),
            static_cast<ULONG>(private_data.size()),
            op, ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }
    this->scheduler_.on_pending(op);
  }

  void start_accept_op(implementation_type& impl, native_qp_t* qp,
                       asio::const_buffer private_data,
                       nd_op_base* op) {
    this->scheduler_.work_started();
    // One-shot: mark non-idle on attempt so any reuse is rejected (rdma_cm-aligned).
    impl.connect_state_ = connect_state::connecting;
    asio::error_code ec{};
    auto const eff = effective_config();
    accept(impl.connector_.Get(), qp,
           eff.inbound_read_limit_,
           eff.outbound_read_limit_,
           private_data.size() == 0 ? nullptr : private_data.data(),
           static_cast<ULONG>(private_data.size()),
           op, ec);
    if (ec) {
      this->scheduler_.on_completion(op, ec);
      return;
    }
    this->scheduler_.on_pending(op);
  }

  nd_device_service& device_svc_;  // cached (registration guard + device + conn params)
};

}
