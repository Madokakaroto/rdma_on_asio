#pragma once

#include "asio/associated_cancellation_slot.hpp"
#include "asio/cancellation_type.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"
#include "rdma/ibv/detail/ibv_ops_cm.hpp"

namespace asio::rdma::detail {

// Per-op cancellation handler for control-plane reactor ops. Removes the op
// (matched by key == op pointer) from the CM event-channel fd's read queue,
// which completes it with operation_aborted. Wires cancellation_slot for
// connect / accept / get_connection / wait_disconnect (Stage 2). It does NOT
// touch connect_state_: connect/accept ops mark themselves terminal on aborted
// completion (do_complete), while wait_disconnect must leave the connection
// alive. See docs/cancellation_stage2_control_single_op.md.
struct cm_op_cancellation {
  asio::detail::reactor* reactor_;
  asio::detail::reactor::per_descriptor_data* reactor_data_;
  int fd_;
  void* key_;

  cm_op_cancellation(asio::detail::reactor* reactor,
                     asio::detail::reactor::per_descriptor_data* reactor_data,
                     int fd, void* key)
      : reactor_(reactor), reactor_data_(reactor_data), fd_(fd), key_(key) {
  }

  void operator()(asio::cancellation_type_t type) {
    if (!!(type & (asio::cancellation_type::terminal |
                   asio::cancellation_type::partial |
                   asio::cancellation_type::total))) {
      reactor_->cancel_ops_by_key(fd_, *reactor_data_,
                                  asio::detail::reactor::read_op, key_);
    }
  }
};

// Wire a cancellation_slot to cancel this reactor op by key (op pointer = key).
// IMPORTANT: capture the slot via get_associated_cancellation_slot(handler)
// BEFORE the op constructor moves the handler -- reading it from a moved-from
// (e.g. awaitable) handler dereferences a null cancellation_state. Call this
// after the op is allocated (need the key) and before it is armed.
template <typename CancellationSlot>
inline void arm_cm_cancellation(
    CancellationSlot slot, asio::detail::reactor& reactor,
    asio::detail::reactor::per_descriptor_data& reactor_data, int fd,
    asio::detail::reactor_op* op) {
  if (slot.is_connected()) {
    op->cancellation_key_ = op;
    slot.template emplace<cm_op_cancellation>(&reactor, &reactor_data, fd, op);
  }
}

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
