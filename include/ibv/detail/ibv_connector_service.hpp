#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>

#include "asio/buffer.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/execution_context.hpp"
#include "asio/io_context.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_device_service.hpp"
#include "ibv/detail/ibv_op_accept.hpp"
#include "ibv/detail/ibv_op_connect.hpp"
#include "ibv/detail/ibv_op_wait_disconnect.hpp"
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
    int timeout_ = default_cm_timeout_ms;
    // Peer's private data (client request on server; server reply on client).
    std::array<std::byte, max_private_data_size> private_data_buffer_{};
    std::size_t private_data_length_ = 0;
    // Teardown arbiter: mirrors the connect/accept op's stage and is the SOLE
    // basis for disconnect()'s decision. Atomic so disconnect() is thread-safe
    // (callable from any thread, no strand). See docs/cancellation_stage1_object.md.
    std::atomic<connect_state> connect_state_{connect_state::idle};
    // Peer-disconnect latch, set by the wait watcher. SEPARATE from connect_state_
    // so a peer disconnect does not push us to `closed` and thereby suppress a
    // later disconnect()'s local WR flush. Makes async_wait_disconnect
    // level-triggered (already-closed -> immediate completion).
    std::atomic<bool> peer_closed_{false};
  };

  explicit ibv_connector_service(asio::execution_context& context)
      : asio::detail::execution_context_service_base<
            ibv_connector_service<PortSpace>>(context)
      , ibv_service_base(context)
      , device_svc_(asio::use_service<ibv_device_service>(context)) {
  }

  void shutdown() {
    base_shutdown<implementation_type>(
        [this](implementation_type& impl) { close_for_destruction(impl); });
  }

  void construct(implementation_type& impl) {
    base_construct(impl);
    impl.cm_reactor_data_ = asio::detail::reactor::per_descriptor_data();
    impl.timeout_ = default_cm_timeout_ms;
    impl.connect_state_.store(connect_state::idle, std::memory_order_relaxed);
    impl.peer_closed_.store(false, std::memory_order_relaxed);
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
    impl.timeout_ = other_impl.timeout_;
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    impl.connect_state_.store(
        other_impl.connect_state_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    impl.peer_closed_.store(
        other_impl.peer_closed_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
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
    impl.timeout_ = other_impl.timeout_;
    impl.private_data_buffer_ = other_impl.private_data_buffer_;
    impl.private_data_length_ = other_impl.private_data_length_;
    impl.connect_state_.store(
        other_impl.connect_state_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    impl.peer_closed_.store(
        other_impl.peer_closed_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    if (impl.cm_channel_) {
      this->reactor_.move_descriptor(impl.cm_channel_->fd, impl.cm_reactor_data_,
                                     other_impl.cm_reactor_data_);
    }
  }

  // --- open / assign (asio basic_socket::open(protocol) / assign) ---

  // Create a fresh event channel + cm_id (client). ps_type = PortSpace::rdma_type().
  // Requires use_device() on this io_context (config is centralized there).
  void open(implementation_type& impl, rdma_port_space ps_type,
            asio::error_code& ec) {
    if (is_open(impl)) {
      ec = make_error_code(ibv_errc::ext_already_registered);
      return;
    }
    if (!device_registered()) {
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
    if (int err = this->reactor_.register_descriptor(channel->fd,
                                                     impl.cm_reactor_data_)) {
      ec = make_system_error_code(err);
      return;
    }
    impl.cm_channel_ = std::move(channel);
    impl.cm_id_ = std::move(cm_id);
    impl.timeout_ = default_cm_timeout_ms;
    impl.connect_state_.store(connect_state::idle, std::memory_order_relaxed);
    impl.peer_closed_.store(false, std::memory_order_relaxed);
    ec.clear();
  }

  // Adopt a connector handle produced by the listener (server). Also store the
  // client's request private data so get_remote_data() can return it.
  void assign(implementation_type& impl, native_connector_type&& handle,
              std::span<const std::byte> remote_pd, asio::error_code& ec) {
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
    impl.timeout_ = default_cm_timeout_ms;
    impl.connect_state_.store(connect_state::idle, std::memory_order_relaxed);
    impl.peer_closed_.store(false, std::memory_order_relaxed);
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

  // NOTE: connector has no public cancel(). Object-level teardown -- including
  // aborting an in-flight async_connect/async_accept -- is connector::disconnect(),
  // which adapts to connect_state_ (see disconnect() below). Only listener keeps
  // cancel() (reusable accept semantics). See docs/cancellation_stage1_object.md.

  // --- async operations ---

  template <typename Handler, typename IoExecutor>
  void async_connect(implementation_type& impl, ibv_create_qp_fn create_qp,
                     endpoint_type const& endpoint,
                     asio::const_buffer private_data, Handler& handler,
                     IoExecutor const& io_ex) {
    // Auto-open (asio socket.connect opens with the endpoint's protocol).
    asio::error_code open_ec;
    if (!is_open(impl)) {
      open(impl, PortSpace::rdma_type(), open_ec);
    }
    // RDMA read/atomic negotiation from the centralized effective config.
    auto const eff = effective_config();
    using op = ibv_connect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{this->success_ec_,         impl.cm_id_.get(),
                       &impl.connect_state_,      impl.timeout_,
                       private_data.data(),       private_data.size(),
                       to_u8(eff.inbound_read_limit_),
                       to_u8(eff.outbound_read_limit_),
                       std::move(create_qp),      pd_sink(impl),
                       handler,                   io_ex};
    if (open_ec) {
      p.p->ec_ = open_ec;
      this->reactor_.post_immediate_completion(p.p, false);
    }
    else if (impl.connect_state_.load(std::memory_order_acquire) !=
             connect_state::idle) {
      // One-shot connector: only a fresh (idle) connector may connect. Any other
      // state -- in-flight, established, or terminal (disconnect/failed connect,
      // -> closed) -- means the cm_id is used/stranded (rdma_cm cm_ids are not
      // reusable). Early-exit with a clear code instead of driving resolve_addr
      // on a non-IDLE cm_id (raw EINVAL); the user must create a fresh connector.
      // See docs/cancellation_stage1_object.md.
      p.p->ec_ = make_error_code(ibv_errc::ext_connector_terminal);
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
    p.p = new (p.v) op{this->success_ec_, impl.cm_id_.get(), &impl.connect_state_,
                       handler, io_ex};
    if (!device_registered()) {
      p.p->ec_ = make_error_code(ibv_errc::ext_device_not_registered);
      this->reactor_.post_immediate_completion(p.p, false);
      p.v = p.p = 0;
      return;
    }
    start_accept_op(impl, std::move(create_qp), private_data, p.p);
    p.v = p.p = 0;
  }

  // Synchronous, non-blocking disconnect (mirrors socket::shutdown/close which
  // are sync). rdma_disconnect transitions the local QP to ERROR and flushes
  // pending WRs -- those WRs complete on the CQ with operation_aborted
  // ASYNCHRONOUSLY (reaped by the poller / poll()), so "disconnect returned"
  // does NOT mean pending data-plane ops have already aborted. Abrupt teardown:
  // not-yet-sent WRs are dropped, not delivered.
  // Unified, thread-safe teardown (replaces connector::cancel()). Adapts to
  // connect_state_: aborts an in-flight connect/accept (cancel_ops only, no
  // rdma_disconnect since the cm_id never reached ESTABLISHED) OR tears down an
  // established connection (cancel_ops + rdma_disconnect). The CAS-to-closed is
  // the arbitration counterpart to the op's connecting->connected CAS: whoever
  // acts second performs the single rdma_disconnect. Callable from any thread.
  // See docs/cancellation_stage1_object.md (design A.5).
  void disconnect(implementation_type& impl, asio::error_code& ec) {
    ec.clear();
    connect_state old = impl.connect_state_.load(std::memory_order_acquire);
    for (;;) {
      if (old == connect_state::closed) {
        return;  // idempotent: already torn down
      }
      if (impl.connect_state_.compare_exchange_weak(
              old, connect_state::closed, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        break;  // success; `old` holds the true prior state (weak retries refresh it)
      }
    }
    switch (old) {
      case connect_state::addr_resolve:
      case connect_state::addr_route:
      case connect_state::connecting:
        // Not established -> abort the in-flight op only. If it establishes
        // after this, the op's ESTABLISHED-CAS fails and it tears down itself.
        if (impl.cm_channel_) {
          this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
        }
        break;
      case connect_state::connected:
        // Established and the op already completed -> we are the second actor:
        // abort the armed wait watcher + tear down (flush WRs + DREQ), once.
        if (impl.cm_channel_) {
          this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
        }
        detail::disconnect(impl.cm_id_.get(), ec);
        break;
      case connect_state::idle:
      default:
        break;  // nothing armed, nothing connected
    }
  }

  // Disconnect NOTIFICATION (on_disconnect). One-shot; armed on demand. If the
  // connection is already torn down (peer_closed_ latch set, or connect_state_
  // == closed from a self disconnect()), completes immediately (level-triggered);
  // otherwise a watcher stays armed on the CM fd until a peer
  // DISCONNECTED/DEVICE_REMOVAL arrives. Completion code: ext_disconnected (peer
  // disconnect) / ext_device_removed (local device gone) -- see D-D.
  template <typename Handler, typename IoExecutor>
  void async_wait_disconnect(implementation_type& impl, Handler& handler,
                             IoExecutor const& io_ex) {
    using op = ibv_wait_disconnect_op<Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{this->success_ec_, impl.cm_channel_.get(),
                       &impl.peer_closed_, handler, io_ex};
    if (impl.peer_closed_.load(std::memory_order_acquire) ||
        impl.connect_state_.load(std::memory_order_acquire) ==
            connect_state::closed) {
      p.p->ec_ = make_error_code(ibv_errc::ext_disconnected);
      this->reactor_.post_immediate_completion(p.p, false);
    }
    else if (!is_open(impl)) {
      p.p->ec_ = asio::error::bad_descriptor;
      this->reactor_.post_immediate_completion(p.p, false);
    }
    else {
      this->reactor_.start_op(asio::detail::reactor::read_op,
                              impl.cm_channel_->fd, impl.cm_reactor_data_, p.p,
                              false, false);
    }
    p.v = p.p = 0;
  }

private:
  // device_service holds the device + effective config (installed by use_device)
  // and answers the registration guard. Cached as a ref in the ctor.
  bool device_registered() { return device_svc_.is_registered(); }
  ibv_config_t effective_config() {
    return device_svc_.is_registered() ? device_svc_.get_effective_config()
                                       : ibv_config_t{};
  }
  static std::uint8_t to_u8(std::uint32_t v) {
    return static_cast<std::uint8_t>(v > 255u ? 255u : v);
  }

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
      // Publish addr_resolve BEFORE arming, so a concurrent disconnect() sees an
      // in-flight op (addr_resolve -> cancel_ops). If disconnect() already won
      // (raced ahead to closed), the CAS fails -> complete aborted, do not arm.
      connect_state e = connect_state::idle;
      if (!impl.connect_state_.compare_exchange_strong(
              e, connect_state::addr_resolve, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        op->ec_ = asio::error::operation_aborted;
        this->reactor_.post_immediate_completion(op, false);
        return;
      }
      op->ec_ = asio::error_code{};
      this->reactor_.start_op(asio::detail::reactor::read_op,
                              impl.cm_channel_->fd, impl.cm_reactor_data_, op,
                              false, false);
    }
    else {
      impl.connect_state_.store(connect_state::closed, std::memory_order_release);
      this->reactor_.post_immediate_completion(op, false);
    }
  }

  void start_accept_op(implementation_type& impl, ibv_create_qp_fn create_qp,
                       asio::const_buffer private_data,
                       asio::detail::reactor_op* op) {
    // Publish connecting BEFORE creating the QP / accepting, so a concurrent
    // disconnect() sees an in-flight op. If disconnect() already won, complete
    // aborted and do not accept.
    connect_state e = connect_state::idle;
    if (!impl.connect_state_.compare_exchange_strong(
            e, connect_state::connecting, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      op->ec_ = asio::error::operation_aborted;
      this->reactor_.post_immediate_completion(op, false);
      return;
    }
    // The child cm_id already has a context: create the QP before accepting.
    if (create_qp) {
      op->ec_ = create_qp(impl.cm_id_.get());
      if (op->ec_) {
        impl.connect_state_.store(connect_state::closed,
                                  std::memory_order_release);
        this->reactor_.post_immediate_completion(op, false);
        return;
      }
    }
    auto const eff = effective_config();
    rdma_conn_param param{};
    param.private_data = private_data.data();
    param.private_data_len = static_cast<std::uint8_t>(private_data.size());
    param.responder_resources = to_u8(eff.inbound_read_limit_);
    param.initiator_depth = to_u8(eff.outbound_read_limit_);
    param.rnr_retry_count = 7;
    if (accept(impl.cm_id_.get(), &param, op->ec_) == 0) {
      op->ec_ = asio::error_code{};
      this->reactor_.start_op(asio::detail::reactor::read_op,
                              impl.cm_channel_->fd, impl.cm_reactor_data_, op,
                              false, false);
    }
    else {
      impl.connect_state_.store(connect_state::closed, std::memory_order_release);
      this->reactor_.post_immediate_completion(op, false);
    }
  }

  void close_for_destruction(implementation_type& impl) {
    if (impl.cm_channel_) {
      this->reactor_.deregister_descriptor(impl.cm_channel_->fd,
                                           impl.cm_reactor_data_, false);
      this->reactor_.cleanup_descriptor_data(impl.cm_reactor_data_);
      // Drain + ack any pending CM events (e.g. DISCONNECTED / TIMEWAIT_EXIT)
      // before rdma_destroy_id, which blocks until reported events are acked.
      // Passing cm_id closes the A.7 race window: if an ESTABLISHED arrived but
      // was never processed (disconnect()'s cancel_ops aborted the op first),
      // drain issues a graceful rdma_disconnect on it before destroy.
      drain_cm_events(impl.cm_channel_.get(), impl.cm_id_.get());
    }
    // The QP is created on cm_id (cm_id->qp) and owned here; destroy it before
    // the cm_id, which in turn must precede the event channel.
    if (impl.cm_id_ && impl.cm_id_->qp) {
      ::rdma_destroy_qp(impl.cm_id_.get());
    }
    impl.cm_id_.reset();
    impl.cm_channel_.reset();
  }

  ibv_device_service& device_svc_;  // cached (registration guard + conn params)
};

}
