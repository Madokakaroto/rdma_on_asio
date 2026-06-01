#pragma once

#include <memory>

#include "asio/detail/io_object_impl.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_completion_queue.hpp"
#include "ibv/detail/ibv_verbs_service.hpp"

namespace asio::rdma {

// Queue pair IO object (mirrors nd_queue_pair). Unlike nd, the QP is created by
// the connector during connect/accept (rdma_cm requires cm_id + resolved
// context); the qp here sets up CQ binding + config and exposes a create-qp
// callback that the connector calls to back-fill the QP.
class ibv_queue_pair {
public:
  using service_type = detail::ibv_verbs_service;

  ibv_queue_pair() = default;
  ~ibv_queue_pair() = default;
  ibv_queue_pair(ibv_queue_pair&&) = default;
  ibv_queue_pair& operator=(ibv_queue_pair&&) = default;
  ibv_queue_pair(ibv_queue_pair const&) = delete;
  ibv_queue_pair& operator=(ibv_queue_pair const&) = delete;

  explicit ibv_queue_pair(asio::io_context& io_ctx, ibv_config_t const& config = {})
      : pimpl_(std::make_unique<impl_type>(0, 0, io_ctx)) {
    asio::error_code ec;
    pimpl_->get_service().open(pimpl_->get_implementation(), config, ec);
    asio::detail::throw_error(ec);
  }

  ibv_queue_pair(asio::io_context& io_ctx, ibv_completion_queue& cq,
                 ibv_config_t const& config = {})
      : pimpl_(std::make_unique<impl_type>(0, 0, io_ctx)) {
    asio::error_code ec;
    pimpl_->get_service().open(pimpl_->get_implementation(),
                               cq.native_handle(), config, ec);
    asio::detail::throw_error(ec);
  }

  void open(asio::io_context& io_ctx, ibv_config_t const& config = {}) {
    asio::error_code ec;
    open(io_ctx, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(asio::io_context& io_ctx, ibv_config_t const& config,
            asio::error_code& ec) {
    if (!pimpl_) {
      pimpl_ = std::make_unique<impl_type>(0, 0, io_ctx);
    }
    pimpl_->get_service().open(pimpl_->get_implementation(), config, ec);
  }

  void open(asio::io_context& io_ctx, ibv_completion_queue& cq,
            ibv_config_t const& config, asio::error_code& ec) {
    if (!pimpl_) {
      pimpl_ = std::make_unique<impl_type>(0, 0, io_ctx);
    }
    pimpl_->get_service().open(pimpl_->get_implementation(),
                               cq.native_handle(), config, ec);
  }

  bool is_open() const noexcept {
    return pimpl_ &&
           pimpl_->get_service().is_open(pimpl_->get_implementation());
  }

  detail::native_qp_t* native_handle() const noexcept {
    if (!pimpl_) return nullptr;
    return pimpl_->get_service().native_handle(pimpl_->get_implementation());
  }

  // Used by the connector to create the QP on its cm_id and back-fill this qp.
  detail::ibv_create_qp_fn make_create_qp_fn() {
    auto* svc = &pimpl_->get_service();
    auto* impl = &pimpl_->get_implementation();
    return [svc, impl](detail::native_cm_id_t* cm_id) {
      return svc->create_qp(*impl, cm_id);
    };
  }

  template <typename ConstBufferSequence, typename WriteToken>
  auto async_send(ConstBufferSequence const& buffers, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
                                void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_send(pimpl_->get_implementation(), bufs,
                                           handler, io_ex);
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_recv(MutableBufferSequence const& buffers, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
                                void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_recv(pimpl_->get_implementation(), bufs,
                                           handler, io_ex);
        },
        token, buffers);
  }

  template <typename ConstBufferSequence, typename WriteToken>
  auto async_write(ConstBufferSequence const& buffers,
                   ibv_remote_addr_t const& remote_addr, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
                                void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_write(pimpl_->get_implementation(), bufs,
                                            remote_addr, handler, io_ex);
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_read(MutableBufferSequence const& buffers,
                  ibv_remote_addr_t const& remote_addr, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
                                void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_read(pimpl_->get_implementation(), bufs,
                                           remote_addr, handler, io_ex);
        },
        token, buffers);
  }

private:
  using impl_type = asio::detail::io_object_impl<service_type>;
  std::unique_ptr<impl_type> pimpl_;
};

}
