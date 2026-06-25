#pragma once

#include <algorithm>
#include <atomic>

#include "asio/buffer.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/nd/detail/nd_op_base.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

class nd_connect_op_base : public nd_op_base {
protected:
  using nd_op_base::status_t;
  enum class stage_t {
    connecting,
    connected,
    done,
    error,
  };

protected:
  stage_t stage_;
  std::atomic<connect_state>* state_;
  // Caller-owned reply buffer passed to async_connect. The op only stores the
  // lightweight Asio buffer view; the caller owns the underlying memory.
  asio::mutable_buffer reply_;
  std::size_t reply_len_ = 0;

public:
  ASIO_DECL nd_connect_op_base(IND2Connector* connector,
                               std::atomic<connect_state>* state,
                               asio::mutable_buffer reply,
                               func_type complete_func);

protected:
  IND2Connector* get_connector() const {
    return static_cast<IND2Connector*>(this->get_overlapped());
  }

  ASIO_DECL static status_t do_process(void* owner, nd_op_base* base,
                                       asio::error_code& ec);

  ASIO_DECL status_t process_complete_connect(void* owner,
                                              asio::error_code& ec);

  ASIO_DECL bool mark_connected(asio::error_code& ec);

  ASIO_DECL void capture_remote_pd();
};

template <typename Handler, typename IoExecutor>
class nd_connect_op final : public nd_connect_op_base {
public:
  ASIO_DEFINE_HANDLER_PTR(nd_connect_op);
  using nd_connect_op_base::status_t;

private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  nd_connect_op(IND2Connector* conncetor, std::atomic<connect_state>* state,
                asio::mutable_buffer reply, Handler& handler,
                const IoExecutor& io_ex)
      : nd_connect_op_base(conncetor, state, reply,
                           &nd_connect_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {}

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& result_ec,
                          std::size_t bytes_transferred) {
   asio::error_code ec = result_ec;
   nd_connect_op* o = static_cast<nd_connect_op*>(base);

   // resume the operation for the pending steps
   auto const complete_status = o->resume_process(owner, ec);
   if (complete_status != status_t::completed) {
     return;
   }

   if (owner && ec && o->state_) {
     o->state_->store(connect_state::closed, std::memory_order_release);
   }

   ptr p = {asio::detail::addressof(o->handler_), o, o};

   ASIO_HANDLER_COMPLETION((*o));

   // Take ownership of the operation's outstanding work.
   asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
       asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

   ASIO_ERROR_LOCATION(ec);

   // Make a copy of the handler so that the memory can be deallocated before
   // the upcall is made. Even if we're not about to make an upcall, a
   // sub-object of the handler may be the true owner of the memory associated
   // with the handler. Consequently, a local copy of the handler is required
   // to ensure that any owning sub-object remains valid until after we have
   // deallocated the memory here.
   asio::detail::binder2<Handler, asio::error_code, std::size_t> handler(
       o->handler_, ec, o->reply_len_);
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

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_op_connect.ipp"
#endif
