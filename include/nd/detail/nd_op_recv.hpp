#pragma once

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

template <mr_mutable_buffer_sequence BufferSequence,
          typename Handler, typename IoExecutor>
class nd_recv_op final : public nd_two_sided_op<BufferSequence> {
 public:
  using base_type = nd_two_sided_op<BufferSequence>;
  using complete_func = typename base_type::complete_func;
  using op_type = typename base_type::op_type;

 private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

 public:
  ASIO_DEFINE_HANDLER_PTR(nd_recv_op);
  nd_recv_op(asio::error_code const& ec, BufferSequence const& buffer_sequence,
             Handler& handler, const IoExecutor& io_ex)
      : base_type(&nd_recv_op::do_complete, ec, buffer_sequence)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

  op_type get_op_type() const noexcept {
    return op_type::post_recv;
  }

 private: 
  static void do_complete(void* owner, nd_verbs_op_base* base,
                          asio::error_code const& ec,
                          std::size_t bytes_transferred) {
    // traits nd_recv_op from base object
    nd_recv_op* o = static_cast<nd_recv_op*>(base);

    // Take ownership of the handler object.
    ptr p = {asio::detail::addressof(o->handler_), o, o};
    ASIO_HANDLER_COMPLETION((*o));

    // Take ownership of the rdma_operation's outstanding work.
    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));
    ASIO_ERROR_LOCATION(o->ec_);

    // Make a copy of the handler so that the memory can be deallocated before
    // the upcall is made. Even if we're not about to make an upcall, a
    // sub-object of the handler may be the true owner of the memory associated
    // with the handler. Consequently, a local copy of the handler is required
    // to ensure that any owning sub-object remains valid until after we have
    // deallocated the memory here.
    asio::detail::binder2<Handler, asio::error_code, std::size_t> handler(
        o->handler_, o->ec_, o->bytes_transferred_);
    p.h = asio::detail::addressof(handler.handler_);
    p.reset();

    // Make the upcall if required.
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