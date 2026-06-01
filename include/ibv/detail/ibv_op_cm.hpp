#pragma once

#include "asio/detail/reactor_op.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_ops_cm.hpp"

namespace asio::rdma::detail {

// Base for all rdma_cm reactor operations (mirrors the deprecated rdma_cm_op).
// Derives from asio::detail::reactor_op so a single op can stay armed on the
// CM event-channel fd across multiple readiness events: do_perform returns
// status::not_done to remain armed, status::done to retire and upcall once.
class ibv_op_cm : public asio::detail::reactor_op {
protected:
  native_event_channel_t* channel_;

  ibv_op_cm(asio::error_code const& success_ec,
            native_event_channel_t* channel, perform_func_type perform_func,
            func_type complete_func)
      : asio::detail::reactor_op(success_ec, perform_func, complete_func)
      , channel_(channel) {
  }

  // Pull one CM event. Returns the stored ec_. On EAGAIN, ec_ stays clear and
  // event_ptr is left empty (no event yet) so the caller returns not_done.
  asio::error_code get_cm_event(unique_rdma_cm_event_ptr& event_ptr) {
    native_cm_event_t* event = nullptr;
    int const rc = detail::get_cm_event(channel_, &event, this->ec_);
    if (rc == 0) {
      event_ptr.reset(event);
    }
    return this->ec_;
  }
};

}
