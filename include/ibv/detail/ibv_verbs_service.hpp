#pragma once

#include "asio/detail/reactor.hpp"
#include "asio/execution_context.hpp"
#include "asio/io_context.hpp"
#include "ibv/ibv_buffer.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_io_completion_service.hpp"
#include "ibv/detail/ibv_op_cq_notify.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"
#include "rdma/detail/rdma_op_read.hpp"
#include "rdma/detail/rdma_op_recv.hpp"
#include "rdma/detail/rdma_op_send.hpp"
#include "rdma/detail/rdma_op_write.hpp"
#include "ibv/detail/ibv_service_base.hpp"

namespace asio::rdma::detail {

// Data-plane service backing ibv_queue_pair. The QP is created on the
// connector's cm_id (via create_qp, called by the connector during
// connect/accept) and owned by the connector; this service holds a raw pointer
// for posting. Completions are dispatched through the shared CQ poller (service
// mode) or the user's poll() (explicit-CQ mode). Mirrors nd_verbs_service.
// Data plane is portspace-agnostic (QP/CQ/MR only), so this service is not
// templated on PortSpace. One instance per io_context; all state is per-impl.
class ibv_verbs_service
    : public asio::detail::execution_context_service_base<ibv_verbs_service>
    , public ibv_service_base {
public:
  struct implementation_type : ibv_service_base::base_implementation_type {
    native_qp_t* qp_ = nullptr;             // owned by the connector (cm_id->qp)
    native_cq_t* cq_ = nullptr;             // CQ the QP is bound to
    native_comp_channel_t* comp_channel_ = nullptr;
    bool service_cq_mode_ = false;          // true => shared CQ via io-completion-service
    ibv_config_t config_;
  };

  explicit ibv_verbs_service(asio::execution_context& context)
      : asio::detail::execution_context_service_base<ibv_verbs_service>(context)
      , ibv_service_base(context) {
  }

  void shutdown() {
    base_shutdown<implementation_type>([](implementation_type&) {});
  }

  void construct(implementation_type& impl) { base_construct(impl); }

  void destroy(implementation_type& impl) { base_destroy(impl); }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    base_move_construct(impl, other_impl);
    impl.qp_ = other_impl.qp_;
    impl.cq_ = other_impl.cq_;
    impl.comp_channel_ = other_impl.comp_channel_;
    impl.service_cq_mode_ = other_impl.service_cq_mode_;
    impl.config_ = other_impl.config_;
    other_impl.qp_ = nullptr;
  }

  void move_assign(implementation_type& impl, ibv_verbs_service&,
                   implementation_type& other_impl) {
    base_destroy(impl);
    base_move_construct(impl, other_impl);
    impl.qp_ = other_impl.qp_;
    impl.cq_ = other_impl.cq_;
    impl.comp_channel_ = other_impl.comp_channel_;
    impl.service_cq_mode_ = other_impl.service_cq_mode_;
    impl.config_ = other_impl.config_;
    other_impl.qp_ = nullptr;
  }

  // open: shared-CQ (service) mode.
  void open(implementation_type& impl, ibv_config_t const& config,
            asio::error_code& ec) {
    auto& io_svc = asio::use_service<ibv_io_completion_service>(this->context());
    if (!io_svc.is_initialized()) {
      ec = make_error_code(ibv_errc::ext_invalid_device);
      return;
    }
    impl.cq_ = io_svc.get_cq();
    impl.comp_channel_ = io_svc.get_comp_channel();
    impl.service_cq_mode_ = true;
    impl.config_ = config;
    ec.clear();
  }

  // open: explicit-CQ (poll) mode.
  void open(implementation_type& impl, native_cq_t* external_cq,
            ibv_config_t const& config, asio::error_code& ec) {
    impl.cq_ = external_cq;
    impl.comp_channel_ = nullptr;
    impl.service_cq_mode_ = false;
    impl.config_ = config;
    ec.clear();
  }

  // Create the QP on cm_id (called by the connector once cm_id has a context).
  asio::error_code create_qp(implementation_type& impl, native_cm_id_t* cm_id) {
    asio::error_code ec;
    auto& io_svc = asio::use_service<ibv_io_completion_service>(this->context());
    auto device = io_svc.get_device();
    if (!device || !device->pd_ || !impl.cq_) {
      ec = make_error_code(ibv_errc::ext_invalid_device);
      return ec;
    }
    auto eff = derive_effective_config(impl.config_, device->attr_);

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

    if (verbs_ops::create_qp(cm_id, device->pd_.get(), attr, ec) == 0) {
      impl.qp_ = cm_id->qp;
    }
    return ec;
  }

  bool is_open(implementation_type const& impl) const {
    return impl.qp_ != nullptr;
  }

  native_qp_t* native_handle(implementation_type const& impl) const noexcept {
    return impl.qp_;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_send(implementation_type& impl, BufferSequence const& buffers,
                  Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_send_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, handler, io_ex};
    start_send_op(impl, p.p);
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_recv(implementation_type& impl, BufferSequence const& buffers,
                  Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_recv_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, handler, io_ex};
    start_recv_op(impl, p.p);
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
    start_read_op(impl, p.p);
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
    start_write_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  asio::error_code success_ec_;

  ibv_sglist_t& get_sglist() {
    static thread_local ibv_sglist_t sglist;
    return sglist;
  }

  template <typename SendOpType>
  void start_send_op(implementation_type& impl, SendOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      post_immediate_completion(op);
      return;
    }
    auto& sglist = get_sglist();
    buffers2sglist(buffers, sglist);
    verbs_ops::post_send(impl.qp_, op, sglist.data(), sglist.size(), 0,
                         op->ec_);
    finish_post(impl, op);
  }

  template <typename RecvOpType>
  void start_recv_op(implementation_type& impl, RecvOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      post_immediate_completion(op);
      return;
    }
    auto& sglist = get_sglist();
    buffers2sglist(buffers, sglist);
    verbs_ops::post_recv(impl.qp_, op, sglist.data(), sglist.size(), op->ec_);
    finish_post(impl, op);
  }

  template <typename ReadOpType>
  void start_read_op(implementation_type& impl, ReadOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      post_immediate_completion(op);
      return;
    }
    auto& sglist = get_sglist();
    buffers2sglist(buffers, sglist);
    auto const& ra = op->get_remote_addr();
    verbs_ops::post_read(impl.qp_, op, sglist.data(), sglist.size(), ra.addr_,
                         ra.token_, 0, op->ec_);
    finish_post(impl, op);
  }

  template <typename WriteOpType>
  void start_write_op(implementation_type& impl, WriteOpType* op) {
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      post_immediate_completion(op);
      return;
    }
    auto& sglist = get_sglist();
    buffers2sglist(buffers, sglist);
    auto const& ra = op->get_remote_addr();
    verbs_ops::post_write(impl.qp_, op, sglist.data(), sglist.size(), ra.addr_,
                          ra.token_, 0, op->ec_);
    finish_post(impl, op);
  }

  void finish_post(implementation_type& impl, rdma_verbs_op_base* op) {
    if (op->ec_) [[unlikely]] {
      post_immediate_completion(op);
    }
    else if (impl.service_cq_mode_) {
      auto& io_svc =
          asio::use_service<ibv_io_completion_service>(this->context());
      io_svc.arm_notify();
    }
    // poll mode: the user drives ibv_completion_queue::poll().
  }

  void post_immediate_completion(rdma_verbs_op_base* op) {
    auto* complete = new ibv_complete_op(op);
    this->scheduler_.post_immediate_completion(complete, false);
  }
};

}
