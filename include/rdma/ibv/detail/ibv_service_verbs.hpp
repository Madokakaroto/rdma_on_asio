#pragma once

#include <concepts>
#include <type_traits>

#include "asio/detail/reactor.hpp"
#include "asio/execution_context.hpp"
#include "asio/io_context.hpp"
#include "rdma/ibv/ibv_buffer.hpp"
#include "rdma/ibv/ibv_completion_queue.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_config_derive.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"
#include "rdma/ibv/detail/ibv_op_complete.hpp"
#include "rdma/ibv/detail/ibv_ops_verbs.hpp"
#include "rdma/detail/rdma_op_read.hpp"
#include "rdma/detail/rdma_op_recv.hpp"
#include "rdma/detail/rdma_op_send.hpp"
#include "rdma/detail/rdma_op_write.hpp"
#include "rdma/ibv/detail/ibv_service_base.hpp"

namespace asio::rdma::detail {

// Data-plane service backing ibv_queue_pair. The per-QP state (implementation_type)
// is owned by the queue_pair (no io_object_impl); this service supplies the verbs
// logic in two flavors:
//   - static methods (poll mode): operate purely on implementation_type, touch no
//     io_context. Immediate completions (empty buffers / sync post errors) are
//     queued on the bound ibv_completion_queue and drained by the user's poll().
//   - member methods (event mode): reachable via use_service on the io_context;
//     arm the shared-CQ poller and route immediate completions to the scheduler.
//
// The QP is created on the connector's cm_id (via the static create_qp, invoked
// by the connector during connect/accept). Data plane is portspace-agnostic.
class ibv_verbs_service
    : public asio::detail::execution_context_service_base<ibv_verbs_service>
    , public ibv_service_base {
public:
  // Owned by the queue_pair. event mode: poll_cq_ == nullptr (completions via the
  // io_completion_service); poll mode: poll_cq_ != nullptr (completions via poll()).
  struct implementation_type {
    native_qp_t* qp_ = nullptr;             // owned by the connector (cm_id->qp)
    native_cq_t* cq_ = nullptr;             // CQ the QP is bound to
    ibv_device_ptr device_;                 // for create_qp (pd)
    ibv_config_t config_;                   // effective config
    ibv_completion_queue* poll_cq_ = nullptr;  // poll-mode immediate-completion sink
  };

  explicit ibv_verbs_service(asio::execution_context& context)
      : asio::detail::execution_context_service_base<ibv_verbs_service>(context)
      , ibv_service_base(context) {
  }

  // The native QP is owned by the connector; the CQ/device by the
  // io_completion_service / completion_queue. Nothing to tear down here.
  void shutdown() {}

  // Create the QP on cm_id (called by the connector once cm_id has a context).
  // Static: device/cq/config all come from the impl, so no io_context is needed.
  static asio::error_code create_qp(implementation_type& impl,
                                    native_cm_id_t* cm_id) {
    asio::error_code ec;
    if (!impl.device_ || !impl.device_->pd_ || !impl.cq_) {
      ec = make_error_code(rdma_errc::invalid_device);
      return ec;
    }
    auto const& eff = impl.config_;

    native_qp_init_attr_t attr{};
    attr.qp_context = nullptr;
    attr.send_cq = impl.cq_;
    attr.recv_cq = impl.cq_;
    attr.srq = nullptr;
    attr.cap.max_send_wr = eff.max_send_wr_;
    attr.cap.max_recv_wr = eff.max_recv_wr_;
    attr.cap.max_send_sge = eff.max_send_sge_;
    attr.cap.max_recv_sge = eff.max_recv_sge_;
    attr.cap.max_inline_data = 0;
    attr.qp_type = IBV_QPT_RC;
    attr.sq_sig_all = 1;

    if (verbs_ops::create_qp(cm_id, impl.device_->pd_.get(), attr, ec) == 0) {
      impl.qp_ = cm_id->qp;
    }
    return ec;
  }

  // "bound" = associated with a completion mechanism (a CQ). Note this is NOT
  // "the native QP exists": on ibv the QP is created later by the connector, so
  // native_handle() can still be null after a successful bind.
  static bool is_bound(implementation_type const& impl) {
    return impl.cq_ != nullptr;
  }

  static native_qp_t* native_handle(implementation_type const& impl) noexcept {
    return impl.qp_;
  }

  // ---- poll mode: static async ops (no io_context) ----

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  static void async_send_static(implementation_type& impl,
                                BufferSequence const& buffers, Handler& handler,
                                IoExecutor const& io_ex) {
    using op = rdma_send_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{asio::error_code{}, buffers, handler, io_ex};
    finish_poll(impl, p.p, do_post_send(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  static void async_recv_static(implementation_type& impl,
                                BufferSequence const& buffers, Handler& handler,
                                IoExecutor const& io_ex) {
    using op = rdma_recv_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{asio::error_code{}, buffers, handler, io_ex};
    finish_poll(impl, p.p, do_post_recv(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  static void async_read_static(implementation_type& impl,
                                BufferSequence const& buffers,
                                ibv_remote_addr_t const& remote_addr,
                                Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_read_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{asio::error_code{}, buffers, remote_addr, handler, io_ex};
    finish_poll(impl, p.p, do_post_read(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  static void async_write_static(implementation_type& impl,
                                 BufferSequence const& buffers,
                                 ibv_remote_addr_t const& remote_addr,
                                 Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_write_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{asio::error_code{}, buffers, remote_addr, handler, io_ex};
    finish_poll(impl, p.p, do_post_write(impl, p.p));
    p.v = p.p = 0;
  }

  // ---- event mode: member async ops (use the io_context) ----

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_send(implementation_type& impl, BufferSequence const& buffers,
                  Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_send_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, handler, io_ex};
    finish_event(impl, p.p, do_post_send(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_recv(implementation_type& impl, BufferSequence const& buffers,
                  Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_recv_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, handler, io_ex};
    finish_event(impl, p.p, do_post_recv(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_read(implementation_type& impl, BufferSequence const& buffers,
                  ibv_remote_addr_t const& remote_addr, Handler& handler,
                  IoExecutor const& io_ex) {
    using op = rdma_read_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, remote_addr, handler, io_ex};
    finish_event(impl, p.p, do_post_read(impl, p.p));
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_write(implementation_type& impl, BufferSequence const& buffers,
                   ibv_remote_addr_t const& remote_addr, Handler& handler,
                   IoExecutor const& io_ex) {
    using op = rdma_write_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, remote_addr, handler, io_ex};
    finish_event(impl, p.p, do_post_write(impl, p.p));
    p.v = p.p = 0;
  }

private:
  asio::error_code success_ec_;

  // Reject a buffer sequence whose SGE count exceeds the device's max_sge before
  // posting -- a clean library error instead of a raw EINVAL from ibv_post_*.
  // Returns true (and sets ec) when over the limit. (sge_max == 0 means the
  // effective config was never derived; treat as "no limit".)
  static bool exceeds_sge_limit(std::size_t sge_count, std::uint32_t sge_max,
                                asio::error_code& ec) {
    if (sge_max != 0 && sge_count > sge_max) {
      ec = make_error_code(rdma_errc::too_many_sge);
      return true;
    }
    return false;
  }

  template <typename BufferSequence>
  static constexpr bool is_single_buffer_sequence_v =
      std::same_as<std::remove_cvref_t<BufferSequence>, const_buffer> ||
      std::same_as<std::remove_cvref_t<BufferSequence>, mutable_buffer>;

  // Post the work request. Returns true if the op completed *immediately* (empty
  // buffer sequence or a synchronous post failure) and so needs the
  // immediate-completion sink; false if it was posted and will complete via a CQE.
  template <typename SendOpType>
  static bool do_post_send(implementation_type& impl, SendOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if constexpr (is_single_buffer_sequence_v<decltype(buffers)>) {
      if (buffers.length() == 0) {
        return true;
      }
      if (exceeds_sge_limit(1, impl.config_.max_send_sge_, op->ec_)) {
        return true;
      }
      native_sge_t sge{};
      fill_native_sge(sge, buffers);
      verbs_ops::post_send(impl.qp_, op, &sge, 1, 0, op->ec_);
      return static_cast<bool>(op->ec_);
    }

    ibv_sglist_t sglist;
    auto built = build_native_sglist(buffers, sglist, impl.config_.max_send_sge_);
    if (built.all_empty) {
      return true;
    }
    if (built.too_many_sge) {
      op->ec_ = make_error_code(rdma_errc::too_many_sge);
      return true;
    }
    verbs_ops::post_send(impl.qp_, op, built.data, built.count, 0, op->ec_);
    return static_cast<bool>(op->ec_);
  }

  template <typename RecvOpType>
  static bool do_post_recv(implementation_type& impl, RecvOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if constexpr (is_single_buffer_sequence_v<decltype(buffers)>) {
      if (buffers.length() == 0) {
        return true;
      }
      if (exceeds_sge_limit(1, impl.config_.max_recv_sge_, op->ec_)) {
        return true;
      }
      native_sge_t sge{};
      fill_native_sge(sge, buffers);
      verbs_ops::post_recv(impl.qp_, op, &sge, 1, op->ec_);
      return static_cast<bool>(op->ec_);
    }

    ibv_sglist_t sglist;
    auto built = build_native_sglist(buffers, sglist, impl.config_.max_recv_sge_);
    if (built.all_empty) {
      return true;
    }
    if (built.too_many_sge) {
      op->ec_ = make_error_code(rdma_errc::too_many_sge);
      return true;
    }
    verbs_ops::post_recv(impl.qp_, op, built.data, built.count, op->ec_);
    return static_cast<bool>(op->ec_);
  }

  template <typename ReadOpType>
  static bool do_post_read(implementation_type& impl, ReadOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if constexpr (is_single_buffer_sequence_v<decltype(buffers)>) {
      if (buffers.length() == 0) {
        return true;
      }
      if (exceeds_sge_limit(1, impl.config_.max_send_sge_, op->ec_)) {
        return true;
      }
      native_sge_t sge{};
      fill_native_sge(sge, buffers);
      auto const& ra = op->get_remote_addr();
      verbs_ops::post_read(impl.qp_, op, &sge, 1, ra.addr_, ra.token_, 0,
                           op->ec_);
      return static_cast<bool>(op->ec_);
    }

    ibv_sglist_t sglist;
    auto built = build_native_sglist(buffers, sglist, impl.config_.max_send_sge_);
    if (built.all_empty) {
      return true;
    }
    if (built.too_many_sge) {
      op->ec_ = make_error_code(rdma_errc::too_many_sge);
      return true;
    }
    auto const& ra = op->get_remote_addr();
    verbs_ops::post_read(impl.qp_, op, built.data, built.count, ra.addr_,
                         ra.token_, 0, op->ec_);
    return static_cast<bool>(op->ec_);
  }

  template <typename WriteOpType>
  static bool do_post_write(implementation_type& impl, WriteOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if constexpr (is_single_buffer_sequence_v<decltype(buffers)>) {
      if (buffers.length() == 0) {
        return true;
      }
      if (exceeds_sge_limit(1, impl.config_.max_send_sge_, op->ec_)) {
        return true;
      }
      native_sge_t sge{};
      fill_native_sge(sge, buffers);
      auto const& ra = op->get_remote_addr();
      verbs_ops::post_write(impl.qp_, op, &sge, 1, ra.addr_, ra.token_, 0,
                            op->ec_);
      return static_cast<bool>(op->ec_);
    }

    ibv_sglist_t sglist;
    auto built = build_native_sglist(buffers, sglist, impl.config_.max_send_sge_);
    if (built.all_empty) {
      return true;
    }
    if (built.too_many_sge) {
      op->ec_ = make_error_code(rdma_errc::too_many_sge);
      return true;
    }
    auto const& ra = op->get_remote_addr();
    verbs_ops::post_write(impl.qp_, op, built.data, built.count, ra.addr_,
                          ra.token_, 0, op->ec_);
    return static_cast<bool>(op->ec_);
  }

  // poll mode: immediate completions go to the CQ's ready queue (drained by
  // poll()); posted ops complete when the user reaps their CQE.
  static void finish_poll(implementation_type& impl, rdma_verbs_op_base* op,
                          bool immediate) {
    if (immediate) {
      impl.poll_cq_->push_ready(op);
    }
  }

  // event mode: a posted op needs nothing here --the io_completion_service's
  // poller is already armed (started at queue_pair::bind) and self-perpetuating,
  // so it will reap this op's CQE. Only an immediate completion (empty buffers /
  // synchronous post error) needs scheduling onto the io_context.
  void finish_event(implementation_type& /*impl*/, rdma_verbs_op_base* op,
                    bool immediate) {
    if (immediate) [[unlikely]] {
      auto* complete = new ibv_complete_op(op);
      this->scheduler_.post_immediate_completion(complete, false);
    }
  }
};

}
