#pragma once

#include "asio/async_result.hpp"
#include "asio/io_context.hpp"
#include "asio/system_executor.hpp"
#include "ibv/ibv_completion_queue.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/detail/ibv_service_device.hpp"
#include "ibv/detail/ibv_service_io_completion.hpp"
#include "ibv/detail/ibv_service_verbs.hpp"

namespace asio::rdma {

// Queue pair IO object. Two mutually-exclusive modes, distinguished by io_ctx_:
//
//   ibv_queue_pair(io_context&)        event mode — completions bridged into the
//                                       io_context via use_device's shared CQ.
//   ibv_queue_pair(completion_queue&)  poll mode  — io_context-free data plane;
//                                       completions reaped by the user's poll().
//
// There is no io_object_impl: the QP owns the verbs-service implementation_type
// directly and selects the static (poll) or member (event) service entry points
// per call. The native QP itself is created/owned by the connector on its cm_id
// (deferred); this object holds a non-owning pointer, so the rule of five is just
// "move impl_ + io_ctx_". Config is read from the holder (use_device /
// completion_queue), not passed here.
class ibv_queue_pair {
public:
  using service_type = detail::ibv_verbs_service;

  ibv_queue_pair() = default;
  ~ibv_queue_pair() = default;

  ibv_queue_pair(ibv_queue_pair&& other) noexcept
      : impl_(std::move(other.impl_))
      , io_ctx_(other.io_ctx_)
      , verbs_svc_(other.verbs_svc_) {
    other.impl_ = impl_type{};
    other.io_ctx_ = nullptr;
    other.verbs_svc_ = nullptr;
  }
  ibv_queue_pair& operator=(ibv_queue_pair&& other) noexcept {
    if (this != &other) {
      impl_ = std::move(other.impl_);
      io_ctx_ = other.io_ctx_;
      verbs_svc_ = other.verbs_svc_;
      other.impl_ = impl_type{};
      other.io_ctx_ = nullptr;
      other.verbs_svc_ = nullptr;
    }
    return *this;
  }
  ibv_queue_pair(ibv_queue_pair const&) = delete;
  ibv_queue_pair& operator=(ibv_queue_pair const&) = delete;

  // event mode: bind to the shared CQ installed by use_device on io_ctx.
  explicit ibv_queue_pair(asio::io_context& io_ctx) {
    asio::error_code ec;
    bind(io_ctx, ec);
    asio::detail::throw_error(ec);
  }

  // poll mode: bind to a standalone completion_queue (no io_context).
  explicit ibv_queue_pair(ibv_completion_queue& cq) { bind(cq); }

  // deferred bind — event mode (the io_context's managed CQ)
  void bind(asio::io_context& io_ctx) {
    asio::error_code ec;
    bind(io_ctx, ec);
    asio::detail::throw_error(ec);
  }

  void bind(asio::io_context& io_ctx, asio::error_code& ec) {
    auto& dev_svc = asio::use_service<detail::ibv_device_service>(io_ctx);
    if (!dev_svc.is_registered()) {
      ec = make_error_code(rdma_errc::device_not_registered);
      return;
    }
    auto& io_svc =
        asio::use_service<detail::ibv_io_completion_service>(io_ctx);
    io_ctx_ = &io_ctx;
    impl_.device_ = dev_svc.get_device();
    impl_.cq_ = io_svc.get_cq();
    impl_.config_ = dev_svc.get_effective_config();
    impl_.poll_cq_ = nullptr;
    // Cache the verbs service once — the event-mode async_* path uses it per op.
    verbs_svc_ = &asio::use_service<detail::ibv_verbs_service>(io_ctx);
    // Start the shared-CQ poller (idempotent); the data plane never arms again.
    io_svc.ensure_poller_started();
    ec.clear();
  }

  // deferred bind — poll mode (a user-owned completion_queue)
  void bind(ibv_completion_queue& cq) {
    io_ctx_ = nullptr;
    verbs_svc_ = nullptr;  // poll mode uses the static service entry points
    impl_.device_ = cq.device();
    impl_.cq_ = cq.native_handle();
    impl_.config_ = cq.effective_config();
    impl_.poll_cq_ = &cq;
  }

  // Bound to a completion mechanism? (Not "native QP exists" — on ibv the QP is
  // created later by the connector; use native_handle() for that.)
  bool is_bound() const noexcept {
    return detail::ibv_verbs_service::is_bound(impl_);
  }

  // Which completion mechanism this QP is bound to.
  completion_mode bound_type() const noexcept {
    if (!is_bound()) return completion_mode::none;
    return io_ctx_ ? completion_mode::event : completion_mode::poll;
  }

  detail::native_qp_t* native_handle() const noexcept {
    return detail::ibv_verbs_service::native_handle(impl_);
  }

  // Used by the connector to create the QP on its cm_id and back-fill this qp.
  detail::ibv_create_qp_fn make_create_qp_fn() {
    auto* impl = &impl_;
    return [impl](detail::native_cm_id_t* cm_id) {
      return detail::ibv_verbs_service::create_qp(*impl, cm_id);
    };
  }

  template <typename ConstBufferSequence, typename WriteToken>
  auto async_send(ConstBufferSequence const& buffers, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
                                void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          if (io_ctx_) {
            (*verbs_svc_)
                .async_send(impl_, bufs, handler, io_ctx_->get_executor());
          } else {
            detail::ibv_verbs_service::async_send_static(
                impl_, bufs, handler, asio::system_executor{});
          }
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_recv(MutableBufferSequence const& buffers, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
                                void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          if (io_ctx_) {
            (*verbs_svc_)
                .async_recv(impl_, bufs, handler, io_ctx_->get_executor());
          } else {
            detail::ibv_verbs_service::async_recv_static(
                impl_, bufs, handler, asio::system_executor{});
          }
        },
        token, buffers);
  }

  template <typename ConstBufferSequence, typename WriteToken>
  auto async_write(ConstBufferSequence const& buffers,
                   ibv_remote_addr_t const& remote_addr, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
                                void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          if (io_ctx_) {
            (*verbs_svc_)
                .async_write(impl_, bufs, remote_addr, handler,
                             io_ctx_->get_executor());
          } else {
            detail::ibv_verbs_service::async_write_static(
                impl_, bufs, remote_addr, handler, asio::system_executor{});
          }
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_read(MutableBufferSequence const& buffers,
                  ibv_remote_addr_t const& remote_addr, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
                                void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          if (io_ctx_) {
            (*verbs_svc_)
                .async_read(impl_, bufs, remote_addr, handler,
                            io_ctx_->get_executor());
          } else {
            detail::ibv_verbs_service::async_read_static(
                impl_, bufs, remote_addr, handler, asio::system_executor{});
          }
        },
        token, buffers);
  }

private:
  using impl_type = detail::ibv_verbs_service::implementation_type;
  impl_type impl_;
  asio::io_context* io_ctx_ = nullptr;             // null => poll mode
  detail::ibv_verbs_service* verbs_svc_ = nullptr;  // cached in bind(io) (event mode)
};

}
