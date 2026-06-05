#pragma once

#include <algorithm>
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
  // Where to store the server's reply private data (after CompleteConnect).
  nd_pd_sink remote_pd_;

public:
  nd_connect_op_base(IND2Connector* connector, nd_pd_sink remote_pd,
                     func_type complete_func)
     : nd_op_base(connector, &nd_connect_op_base::do_process, complete_func)
     , stage_(stage_t::connecting)
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
    auto const hr = get_connector()->CompleteConnect(this);
    if (hr != ND_SUCCESS && hr != ND_PENDING) {
      ec = static_cast<nd_errc>(hr);
      stage_ = stage_t::error;
      return status_t::completed;
    }
   stage_ = stage_t::connected;
    return status_t::continuation;
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
  nd_connect_op(IND2Connector* conncetor, nd_pd_sink remote_pd, Handler& handler,
                const IoExecutor& io_ex)
      : nd_connect_op_base(conncetor, remote_pd, &nd_connect_op::do_complete)
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

template <typename Handler, typename IoExecutor>
class nd_disconnect_op final : public nd_op_base {
private:
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ASIO_DEFINE_HANDLER_PTR(nd_disconnect_op);
  nd_disconnect_op(IND2Connector* conncetor, Handler& handler,
                   const IoExecutor& io_ex)
      : nd_op_base(conncetor, &nd_op_base::default_process,
                   &nd_disconnect_op::do_complete)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {}

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& result_ec,
                          std::size_t /*bytes_transferred*/) {
   asio::error_code ec = result_ec;

   nd_disconnect_op* o = static_cast<nd_disconnect_op*>(base);
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

// Fire-and-forget disconnect op (no handler): backs the synchronous, non-blocking
// connector::disconnect(). ND2 Disconnect is overlapped, so we issue it with this
// self-reaping op and return without waiting; when IOCP completes the overlapped,
// do_complete simply frees the op (no user upcall). Mirrors nd_poll_wc_op's
// self-perpetuating/self-reaping pattern.
class nd_disconnect_ff_op final : public nd_op_base {
public:
  struct Handler {};
  ASIO_DEFINE_HANDLER_PTR(nd_disconnect_ff_op);

  explicit nd_disconnect_ff_op(IND2Connector* connector)
      : nd_op_base(connector, &nd_op_base::default_process,
                   &nd_disconnect_ff_op::do_complete) {
  }

private:
  static void do_complete(void* /*owner*/, asio::detail::operation* base,
                          const asio::error_code& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    auto* o = static_cast<nd_disconnect_ff_op*>(base);
    ptr p = {nullptr, o, o};
    p.reset();  // fire-and-forget: free the op, no upcall
  }
};

// Disconnect-notification watcher (on_disconnect). Issues IND2Connector::
// NotifyDisconnect; when that overlapped completes (connection disconnected), it
// sets the connector's disconnected_ flag and upcalls handler(ext_disconnected).
// Mirrors nd_disconnect_op. [Windows verify] NotifyDisconnect completion semantics.
template <typename Handler, typename IoExecutor>
class nd_wait_disconnect_op final : public nd_op_base {
private:
  bool* disconnected_;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

public:
  ASIO_DEFINE_HANDLER_PTR(nd_wait_disconnect_op);
  nd_wait_disconnect_op(IND2Connector* connector, bool* disconnected,
                        Handler& handler, const IoExecutor& io_ex)
      : nd_op_base(connector, &nd_op_base::default_process,
                   &nd_wait_disconnect_op::do_complete)
      , disconnected_(disconnected)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    nd_wait_disconnect_op* o = static_cast<nd_wait_disconnect_op*>(base);
    if (o->disconnected_) {
      *o->disconnected_ = true;
    }
    // NotifyDisconnect completing means the connection is gone: report a single
    // disconnect code (not mapped to a socket error). See D-D.
    asio::error_code ec = make_error_code(nd_errc::ext_disconnected);

    ptr p = {asio::detail::addressof(o->handler_), o, o};

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(ASIO_MOVE_CAST2(
        asio::detail::handler_work<Handler, IoExecutor>)(o->work_));

    ASIO_ERROR_LOCATION(ec);

    asio::detail::binder1<Handler, asio::error_code> handler(o->handler_, ec);
    p.h = asio::detail::addressof(handler.handler_);
    p.reset();

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
