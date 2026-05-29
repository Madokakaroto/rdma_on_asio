#pragma once

#include "nd/detail/nd_op_base.hpp"

namespace asio::rdma::detail {

// notify work request for IND2CompletionQueue
class nd_notify_wr_op final : public asio::detail::operation {
public:
  static constexpr std::size_t poll_wcs_count = 4;
  using base_type = asio::detail::operation;
  struct Handler {};
  ASIO_DEFINE_HANDLER_PTR(nd_notify_wr_op);

 private:
  nd_connector_state_ptr state_;

 public:
  explicit nd_notify_wr_op(nd_connector_state_ptr const& state)
      : base_type(&nd_notify_wr_op::do_complete)
      , state_(state) {
  }

 private:
  static nd_verbs_op_base* resolve_wc(native_wc_t const& result) {
    assert(result.RequestContext);
    nd_verbs_op_base* op =
        reinterpret_cast<nd_verbs_op_base*>(result.RequestContext);
    op->ec_ = static_cast<nd_errc>(result.Status);
    if (!op->ec_) {
      // send and write's bytes_transferred has been set in op
      if (result.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeSend &&
          result.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeWrite) {
        op->bytes_transferred_ = result.BytesTransferred;
      }
    }
    else {
      op->bytes_transferred_ = 0;
    }
    return op;
  }

  static void resolve_wcs(
      nd2_completion_queue_ptr& cq,
      asio::detail::op_queue<nd_verbs_op_base>& verbs_op_queue) {
    ULONG elem_retrived = 0UL;
    do {
      std::array<native_wc_t, poll_wcs_count> results{};
      elem_retrived = verbs_ops::poll_cq(cq.Get(), results);
      std::ranges::for_each_n(results.begin(), elem_retrived,
        [&](auto const& wc){
        auto* verbs_op = resolve_wc(wc);
        verbs_op_queue.push(verbs_op);
      });
    } while (elem_retrived != 0);
  }

  static void process_wcs(
      void* owner, asio::detail::op_queue<nd_verbs_op_base>& verbs_op_queue) {
    assert(owner);
    auto* wc_op = verbs_op_queue.front();

    while (wc_op) {
      // since the complete function will destroy the memory, we cache it first
      auto* wc_to_process = wc_op;

      // move to the next complete verbs operation
      wc_op = asio::detail::op_queue_access::next(wc_op);

      // pop the operation nor the op_queue will destroy the operation twise
      verbs_op_queue.pop();

      // callback & destroy the verbs operation
      wc_to_process->complete(owner);
    }
  }

  static void process_wcs(void* owner, nd2_completion_queue_ptr& cq) {
    assert(owner);
    asio::detail::op_queue<nd_verbs_op_base> complete_ops{};
    resolve_wcs(cq, complete_ops);
    process_wcs(owner, complete_ops);
  }

  static void do_complete(void* owner, base_type* base,
                          asio::error_code const& result_ec,
                          [[maybe_unused]] std::size_t bytes_transferred) {
    // cast to the dirived operation type
    auto* o = static_cast<nd_notify_wr_op*>(base);
    ptr p = {nullptr, o, o};

    // Take the ownership of the cq object
    auto connector_state = std::move(o->state_);

    // free the operation memory, just as the asio does
    p.reset();

    // process error for notify event
    if (result_ec) {
      // TODO ...
    }

    // do callback
    if (owner) {
      // TODO ... maybe to schedule to another io context
      process_wcs(owner, connector_state->cq_);
    }
  }
};

}