#pragma once

#include "asio/io_context.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "nd/detail/nd_verbs_service.hpp"
#include "nd/nd_completion_queue.hpp"

namespace asio::rdma {

class nd_queue_pair {
public:
  using service_type = detail::nd_verbs_service;

  nd_queue_pair() = default;
  ~nd_queue_pair() = default;
  nd_queue_pair(nd_queue_pair&&) = default;
  nd_queue_pair& operator=(nd_queue_pair&&) = default;
  nd_queue_pair(nd_queue_pair const&) = delete;
  nd_queue_pair& operator=(nd_queue_pair const&) = delete;

  explicit nd_queue_pair(asio::io_context& io_ctx,
                         nd_config_t const& config = {})
      : pimpl_(std::make_unique<impl_type>(0, 0, io_ctx)) {
    asio::error_code ec;
    pimpl_->get_service().open(pimpl_->get_implementation(), config, ec);
    asio::detail::throw_error(ec);
  }

  nd_queue_pair(asio::io_context& io_ctx, nd_completion_queue& cq,
                nd_config_t const& config = {})
      : pimpl_(std::make_unique<impl_type>(0, 0, io_ctx)) {
    asio::error_code ec;
    pimpl_->get_service().open(pimpl_->get_implementation(),
                               cq.native_handle(), config, ec);
    asio::detail::throw_error(ec);
  }

  // delayed init (IOCP mode)
  void open(asio::io_context& io_ctx, nd_config_t const& config = {}) {
    asio::error_code ec;
    open(io_ctx, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(asio::io_context& io_ctx, nd_config_t const& config,
            asio::error_code& ec) {
    if (!pimpl_) {
      pimpl_ = std::make_unique<impl_type>(0, 0, io_ctx);
    }
    pimpl_->get_service().open(pimpl_->get_implementation(), config, ec);
  }

  // delayed init (Poll mode)
  void open(asio::io_context& io_ctx, nd_completion_queue& cq,
            nd_config_t const& config = {}) {
    asio::error_code ec;
    open(io_ctx, cq, config, ec);
    asio::detail::throw_error(ec);
  }

  void open(asio::io_context& io_ctx, nd_completion_queue& cq,
            nd_config_t const& config, asio::error_code& ec) {
    if (!pimpl_) {
      pimpl_ = std::make_unique<impl_type>(0, 0, io_ctx);
    }
    pimpl_->get_service().open(pimpl_->get_implementation(),
                               cq.native_handle(), config, ec);
  }

  bool is_open() const noexcept {
    return pimpl_ && pimpl_->get_service().is_open(
                         pimpl_->get_implementation());
  }

  detail::native_qp_t* native_handle() const noexcept {
    if (!pimpl_) return nullptr;
    return pimpl_->get_service().native_handle(pimpl_->get_implementation());
  }

  // async data operations
  template <typename ConstBufferSequence, typename WriteToken>
  auto async_send(ConstBufferSequence const& buffers, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
        void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          using handler_type = decltype(handler);
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_send(
              pimpl_->get_implementation(), bufs,
              handler, io_ex);
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_recv(MutableBufferSequence const& buffers, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
        void(asio::error_code, std::size_t)>(
        [this](auto handler, auto const& bufs) {
          using handler_type = decltype(handler);
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_recv(
              pimpl_->get_implementation(), bufs,
              handler, io_ex);
        },
        token, buffers);
  }

  template <typename ConstBufferSequence, typename WriteToken>
  auto async_write(ConstBufferSequence const& buffers,
                   nd_remote_addr_t const& remote_addr, WriteToken&& token) {
    return asio::async_initiate<WriteToken,
        void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          using handler_type = decltype(handler);
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_write(
              pimpl_->get_implementation(), bufs, remote_addr,
              handler, io_ex);
        },
        token, buffers);
  }

  template <typename MutableBufferSequence, typename ReadToken>
  auto async_read(MutableBufferSequence const& buffers,
                  nd_remote_addr_t const& remote_addr, ReadToken&& token) {
    return asio::async_initiate<ReadToken,
        void(asio::error_code, std::size_t)>(
        [this, &remote_addr](auto handler, auto const& bufs) {
          using handler_type = decltype(handler);
          auto io_ex = pimpl_->get_executor();
          pimpl_->get_service().async_read(
              pimpl_->get_implementation(), bufs, remote_addr,
              handler, io_ex);
        },
        token, buffers);
  }

private:
  using impl_type = asio::detail::io_object_impl<service_type>;
  std::unique_ptr<impl_type> pimpl_;
};

}
