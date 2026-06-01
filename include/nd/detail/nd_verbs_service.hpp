#pragma once

#include "asio/detail/config.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_service_base.hpp"
#include "nd/detail/nd_io_completion_service.hpp"
#include "nd/detail/nd_ops_verbs.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "nd/detail/nd_op_notify_wr.hpp"
#include "nd/detail/nd_op_complete.hpp"
#include "rdma/detail/rdma_op_send.hpp"
#include "rdma/detail/rdma_op_recv.hpp"
#include "rdma/detail/rdma_op_read.hpp"
#include "rdma/detail/rdma_op_write.hpp"
#include "nd/nd_buffer.hpp"

namespace asio::rdma::detail {

template <typename PortSpace>
class nd_verbs_service
    : public asio::detail::execution_context_service_base<
          nd_verbs_service<PortSpace>>
    , public nd_service_base {
public:
  using base_type = asio::detail::execution_context_service_base<
      nd_verbs_service<PortSpace>>;

  struct implementation_type : nd_service_base::base_implementation_type {
    nd2_queue_pair_ptr qp_;
    native_cq_t* cq_ = nullptr;
    bool iocp_mode_ = false;
    nd_config_t config_;
  };

  explicit nd_verbs_service(asio::execution_context& ctx)
      : base_type(ctx)
      , nd_service_base(ctx)
      , success_ec_() {
  }

  ~nd_verbs_service() = default;

  void shutdown() override {
    base_shutdown<implementation_type>([](implementation_type& impl) {
      impl.qp_.Reset();
      impl.cq_ = nullptr;
    });
  }

  // lifecycle
  void construct(implementation_type& impl) {
    nd_service_base::base_construct(impl);
    impl.qp_.Reset();
    impl.cq_ = nullptr;
    impl.iocp_mode_ = false;
  }

  void destroy(implementation_type& impl) {
    impl.qp_.Reset();
    impl.cq_ = nullptr;
    nd_service_base::base_destroy(impl);
  }

  void move_construct(implementation_type& impl,
                      implementation_type& other_impl) {
    nd_service_base::base_move_construct(impl, other_impl);
    impl.qp_ = std::move(other_impl.qp_);
    impl.cq_ = other_impl.cq_;
    impl.iocp_mode_ = other_impl.iocp_mode_;
    impl.config_ = other_impl.config_;
    other_impl.cq_ = nullptr;
    other_impl.iocp_mode_ = false;
  }

  void move_assign(implementation_type& impl,
                   nd_verbs_service& other_service,
                   implementation_type& other_impl) {
    impl.qp_.Reset();
    nd_service_base::base_destroy(impl);
    nd_service_base::base_construct(impl);
    impl.qp_ = std::move(other_impl.qp_);
    impl.cq_ = other_impl.cq_;
    impl.iocp_mode_ = other_impl.iocp_mode_;
    impl.config_ = other_impl.config_;
    other_impl.cq_ = nullptr;
    other_impl.iocp_mode_ = false;
  }

  // open (IOCP mode): uses CQ from nd_io_completion_service
  void open(implementation_type& impl, nd_config_t const& config,
            asio::error_code& ec) {
    if (impl.qp_) {
      ec = asio::error::already_open;
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

    auto adapter = io_svc.get_device();
    auto const effective = derive_effective_config(config, adapter->info_);

    native_qp_init_attr qp_init_attr{
        .qp_context_ = nullptr,
        .rcq_ = io_svc.get_cq(),
        .icq_ = io_svc.get_cq(),
        .max_send_wr_ = effective.max_send_wr_,
        .max_recv_wr_ = effective.max_recv_wr_,
        .max_send_sge_ = effective.max_send_sge_,
        .max_recv_sge_ = effective.max_recv_sge_,
        .max_inline_data_ = effective.max_inline_data_,
    };
    impl.qp_.Attach(verbs_ops::create_qp(adapter->pd_.get(), qp_init_attr, ec));
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.cq_ = io_svc.get_cq();
    impl.iocp_mode_ = true;
    impl.config_ = effective;
  }

  // open (Poll mode): uses external CQ
  void open(implementation_type& impl, native_cq_t* external_cq,
            nd_config_t const& config, asio::error_code& ec) {
    if (impl.qp_) {
      ec = asio::error::already_open;
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
    if (!external_cq) {
      ec = nd_errc::ext_invalid_connector;
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    auto adapter = io_svc.get_device();
    auto const effective = derive_effective_config(config, adapter->info_);

    native_qp_init_attr qp_init_attr{
        .qp_context_ = nullptr,
        .rcq_ = external_cq,
        .icq_ = external_cq,
        .max_send_wr_ = effective.max_send_wr_,
        .max_recv_wr_ = effective.max_recv_wr_,
        .max_send_sge_ = effective.max_send_sge_,
        .max_recv_sge_ = effective.max_recv_sge_,
        .max_inline_data_ = effective.max_inline_data_,
    };
    impl.qp_.Attach(verbs_ops::create_qp(adapter->pd_.get(), qp_init_attr, ec));
    if (ec) {
      ASIO_ERROR_LOCATION(ec);
      return;
    }

    impl.cq_ = external_cq;
    impl.iocp_mode_ = false;
    impl.config_ = effective;
  }

  bool is_open(implementation_type const& impl) const noexcept {
    return impl.qp_ != nullptr;
  }

  native_qp_t* native_handle(implementation_type const& impl) const noexcept {
    return impl.qp_.Get();
  }

  // async verbs operations
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
                  nd_remote_addr_t const& remote_addr,
                  Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_read_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, remote_addr, handler, io_ex};
    start_read_op(impl, p.p);
    p.v = p.p = 0;
  }

  template <typename BufferSequence, typename Handler, typename IoExecutor>
  void async_write(implementation_type& impl, BufferSequence const& buffers,
                   nd_remote_addr_t const& remote_addr,
                   Handler& handler, IoExecutor const& io_ex) {
    using op = rdma_write_op<BufferSequence, Handler, IoExecutor>;
    typename op::ptr p = {asio::detail::addressof(handler),
                          op::ptr::allocate(handler), 0};
    p.p = new (p.v) op{success_ec_, buffers, remote_addr, handler, io_ex};
    start_write_op(impl, p.p);
    p.v = p.p = 0;
  }

private:
  asio::error_code success_ec_;

  nd_sglist_t& get_sglist() {
    static thread_local nd_sglist_t static_sg_list;
    return static_sg_list;
  }

  template <typename SendOpType>
  void start_send_op(implementation_type& impl, SendOpType* op) {
    assert(op);
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      return;
    }

    nd_sglist_t& sglist = get_sglist();
    buffers2sglist(buffers, sglist);

    verbs_ops::post_send(impl.qp_.Get(), op, sglist.data(),
                         sglist.size(), 0, op->ec_);
    if (op->ec_) [[unlikely]] {
      post_immediate_completion(op);
    } else {
      work_started(impl, op);
    }
  }

  template <typename RecvOpType>
  void start_recv_op(implementation_type& impl, RecvOpType* op) {
    assert(op);
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      return;
    }

    nd_sglist_t& sglist = get_sglist();
    buffers2sglist(buffers, sglist);

    verbs_ops::post_recv(impl.qp_.Get(), op, sglist.data(),
                         sglist.size(), op->ec_);
    if (op->ec_) [[unlikely]] {
      post_immediate_completion(op);
    } else {
      work_started(impl, op);
    }
  }

  template <typename ReadOpType>
  void start_read_op(implementation_type& impl, ReadOpType* op) {
    assert(op);
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      return;
    }

    nd_sglist_t& sglist = get_sglist();
    buffers2sglist(buffers, sglist);

    auto const& remote_addr = op->get_remote_addr();
    verbs_ops::post_read(impl.qp_.Get(), op, sglist.data(),
                         sglist.size(), remote_addr.addr_, remote_addr.token_,
                         0, op->ec_);
    if (op->ec_) [[unlikely]] {
      post_immediate_completion(op);
    } else {
      work_started(impl, op);
    }
  }

  template <typename WriteOpType>
  void start_write_op(implementation_type& impl, WriteOpType* op) {
    assert(op);
    auto const& buffers = op->get_buffer_sequence();
    if (all_empty(buffers)) {
      return;
    }

    nd_sglist_t& sglist = get_sglist();
    buffers2sglist(buffers, sglist);

    auto const& remote_addr = op->get_remote_addr();
    verbs_ops::post_write(impl.qp_.Get(), op, sglist.data(),
                          sglist.size(), remote_addr.addr_, remote_addr.token_,
                          0, op->ec_);
    if (op->ec_) [[unlikely]] {
      post_immediate_completion(op);
    } else {
      work_started(impl, op);
    }
  }

  void work_started(implementation_type& impl, rdma_verbs_op_base* op) {
    if (!impl.iocp_mode_) {
      return;
    }
    auto& io_svc =
        asio::use_service<nd_io_completion_service>(this->context());
    nd_notify_wr_op::Handler handler{};
    nd_notify_wr_op::ptr p = {asio::detail::addressof(handler),
                              nd_notify_wr_op::ptr::allocate(handler), 0};
    p.p = new (p.v) nd_notify_wr_op{get_notify_state(impl)};
    asio::error_code ec;
    io_svc.arm_notify(p.p, ec);
    p.v = p.p = nullptr;
  }

  void post_immediate_completion(rdma_verbs_op_base* error_op) {
    nd_complete_op::Handler handler{};
    nd_complete_op::ptr p = {asio::detail::addressof(handler),
                             nd_complete_op::ptr::allocate(handler), 0};
    p.p = new (p.v) nd_complete_op{error_op};
    this->scheduler_.post_immediate_completion(p.p, false);
    p.v = p.p = 0;
  }

  nd_connector_state_ptr get_notify_state(implementation_type& impl) {
    // nd_notify_wr_op currently requires nd_connector_state_ptr to access cq.
    // TODO: refactor nd_notify_wr_op to accept native_cq_t* directly
    // For now, create a minimal shared state to satisfy the interface.
    auto state = std::make_shared<nd_connector_state_t>();
    state->cq_ = nd2_completion_queue_ptr{};
    state->cq_.Attach(impl.cq_);
    if (impl.cq_) impl.cq_->AddRef();
    return state;
  }
};

}
