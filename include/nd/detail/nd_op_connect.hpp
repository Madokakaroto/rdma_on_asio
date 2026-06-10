#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
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
  // Where to store the server's reply private data (after CompleteConnect).
  nd_pd_sink remote_pd_;

public:
  nd_connect_op_base(IND2Connector* connector,
                     std::atomic<connect_state>* state,
                     nd_pd_sink remote_pd, func_type complete_func)
     : nd_op_base(connector, &nd_connect_op_base::do_process, complete_func)
     , stage_(stage_t::connecting)
     , state_(state)
     , remote_pd_(remote_pd) {
  }

protected:
  IND2Connector* get_connector() const {
    return static_cast<IND2Connector*>(this->get_overlapped());
  }

  static status_t do_process(void* owner, nd_op_base* base, asio::error_code& ec) {
    auto* o = static_cast<nd_connect_op_base*>(base);
    switch (o->stage_) {
      case stage_t::connecting:
        return o->process_complete_connect(owner, ec);
      case stage_t::connected:
        if (!o->mark_connected(ec)) {
          return status_t::completed;
        }
        // Capture server's reply private data into the connector's buffer.
        o->capture_remote_pd();
        o->stage_ = stage_t::done;
        return status_t::completed;
      default:
        ec = nd_errc::ext_already_stopt;
        o->stage_ = stage_t::error;
        return status_t::completed;
    }
  }

  status_t process_complete_connect(void* owner, asio::error_code& ec) {
    this->reset();
    auto const hr = get_connector()->CompleteConnect(this);
    if (FAILED(hr)) {
      ec = static_cast<nd_errc>(hr);
      if (state_) state_->store(connect_state::closed, std::memory_order_release);
      stage_ = stage_t::error;
      return status_t::completed;
    }

    stage_ = stage_t::connected;
    if (hr == ND_PENDING) {
      return status_t::continuation;
    }

    if (!mark_connected(ec)) {
      return status_t::completed;
    }
    capture_remote_pd();
    stage_ = stage_t::done;
    return status_t::completed;
  }

  bool mark_connected(asio::error_code& ec) {
    if (state_) {
      connect_state expected = connect_state::connecting;
      if (!state_->compare_exchange_strong(
              expected, connect_state::connected,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        ec = asio::error::operation_aborted;
        stage_ = stage_t::error;
        return false;
      }
    }
    return true;
  }

  void capture_remote_pd() {
    if (!remote_pd_.buf || !remote_pd_.len || remote_pd_.cap == 0) {
      return;
    }
    void const* pd_ptr = nullptr;
    ULONG pd_size = 0;
    auto const hr = get_connector()->GetPrivateData(&pd_ptr, &pd_size);
    if (SUCCEEDED(hr) && pd_ptr && pd_size > 0) {
      std::size_t n = (std::min)(static_cast<std::size_t>(pd_size),
                                 remote_pd_.cap);
      std::memcpy(remote_pd_.buf, pd_ptr, n);
      *remote_pd_.len = n;
    }
  }
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
                nd_pd_sink remote_pd, Handler& handler, const IoExecutor& io_ex)
      : nd_connect_op_base(conncetor, state, remote_pd,
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
   asio::detail::binder1<Handler, asio::error_code> handler(o->handler_, ec);
   p.h = asio::detail::addressof(handler.handler_);
   p.reset();

   // Make the upcall if required.
   if (owner) {
     asio::detail::fenced_block b(asio::detail::fenced_block::half);
     ASIO_HANDLER_INVOCATION_BEGIN((handler.arg1_));
     w.complete(handler, handler.handler_);
     ASIO_HANDLER_INVOCATION_END;
   }
  }
};

}

#include "asio/detail/pop_options.hpp"
