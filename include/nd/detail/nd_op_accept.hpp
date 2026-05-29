#pragma once

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

class nd_accept_op_base : public nd_op_base {
protected:
  using nd_op_base::status_t;
  enum class stage_t {
    request,
    accept,
    done,
    error,
  };

protected:
  stage_t stage_;
  nd_device_ptr device_;
  nd_connector_state_ptr connector_state_;

public:
  nd_accept_op_base(IND2Listener* listener,
                    func_type complete_func) 
    : nd_op_base(listener, &nd_accept_op_base::do_process, complete_func) 
    , stage_(stage_t::request){
    assert(listener);
  }

  nd_device_ptr& get_device() {
    return device_;
  }

  nd_connector_state_ptr& get_state() {
    return connector_state_;
  }

protected:
  static status_t do_process(void* owner, nd_op_base* base, asio::error_code& ec) {
   nd_accept_op_base* o = static_cast<nd_accept_op_base*>(base);
   switch (o->stage_) {
     case stage_t::request:
       return o->process_accept(owner, ec);
     case stage_t::accept:
       o->stage_ = stage_t::done;
       return status_t::completed;
     default:
       ec = nd_errc::ext_already_stopt;
       o->stage_ = stage_t::error;
       return status_t::completed;
   }
  }

  status_t process_accept(void* owner, asio::error_code& ec) {
    auto* connector = connector_state_->connector_.Get();
    auto* qp = connector_state_->qp_.Get();
    auto const ibr_limit = connector_state_->config_.inbound_read_limit_;
    auto const obr_limit = connector_state_->config_.outbound_read_limit_;
    accept(connector, qp, ibr_limit, obr_limit, nullptr, 0, this, ec);
    if (ec) {
      stage_ = stage_t::error;
      return status_t::completed;
    }
    stage_ = stage_t::accept;
    return status_t::continuation;
  }
};

template <typename Connection, typename Handler, typename IoExecutor>
class nd_accept_op final : public nd_accept_op_base {
 private:
  Connection& peer_;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

 public:
  ASIO_DEFINE_HANDLER_PTR(nd_accept_op);
  nd_accept_op(IND2Listener* listener, Connection& peer,
               Handler& handler, const IoExecutor& io_ex)
      : nd_accept_op_base(listener, &nd_accept_op::do_complete)
      , peer_(peer)
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

 private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& result_ec,
                          std::size_t bytes_transferred) {
    asio::error_code ec = result_ec;
    nd_accept_op* o = static_cast<nd_accept_op*>(base);

    // resume the operation for the pending steps
    auto const complete_status = o->resume_process(owner, ec);
    if (complete_status != status_t::completed) {
      return;
    }

    if (!ec) {
      o->peer_.assign(o->get_device(), o->get_state(), ec);
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

// TODO ... move accept op
}