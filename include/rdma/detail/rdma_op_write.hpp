#pragma once

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

template <mr_const_buffer_sequence BufferSequence, typename Handler,
          typename IoExecutor>
class rdma_write_op final : public rdma_one_sided_op<BufferSequence> {
public:
  using base_type = rdma_one_sided_op<BufferSequence>;
  using op_type = typename base_type::op_type;

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ASIO_DEFINE_HANDLER_PTR(rdma_write_op);
  rdma_write_op(asio::error_code const& ec,
                BufferSequence const& buffer_sequence,
                rdma_remote_addr_t const& remote_addr, Handler& handler,
                IoExecutor const& io_ex)
      : base_type(&rdma_write_op::do_complete, ec, buffer_sequence, remote_addr)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

  op_type get_op_type() const noexcept { return op_type::remote_write; }

private:
  static void do_complete(void* owner, rdma_verbs_op_base* base,
                          asio::error_code const& ec,
                          std::size_t /*bytes_transferred*/) {
    rdma_write_op* o = static_cast<rdma_write_op*>(base);
    if (!ec) {
      o->bytes_transferred_ = buffer_size(o->get_buffer_sequence());
    }

    ptr p = {asio::detail::addressof(o->handler_), o, o};
    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));
    ASIO_ERROR_LOCATION(o->ec_);

    asio::detail::binder2<Handler, asio::error_code, std::size_t> handler(
        o->handler_, o->ec_, o->bytes_transferred_);
    p.h = asio::detail::addressof(handler.handler_);
    p.reset();

    if (owner) {
      asio::detail::fenced_block b(asio::detail::fenced_block::half);
      ASIO_HANDLER_INVOCATION_BEGIN((handler.arg1_, handler.arg2_));
      w.complete(handler, handler.handler_);
      ASIO_HANDLER_INVOCATION_END;
    }
  }
};

}

#include "asio/detail/pop_options.hpp"
