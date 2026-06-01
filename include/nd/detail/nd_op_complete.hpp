#pragma once

#include "nd/detail/nd_op_base.hpp"

namespace asio::rdma::detail {

// The nd_verbs_op class is not the sub class of asio::detail::operation,
// aka. the win_iocp_operation class, which is inherited from OVERLAPPED.
// Thus, nd_verbs_op needs a wrapper class, what is inherited from OVERLAPPED,
// to be enqueued into the IOCP completion queue.
// The nd_complete_op actually covers this.
class nd_complete_op final : public asio::detail::operation {
public:
  struct Handler {};
  ASIO_DEFINE_HANDLER_PTR(nd_complete_op);

private:
  rdma_verbs_op_base* op_;

public:
  explicit nd_complete_op(rdma_verbs_op_base* verbs_op)
      : asio::detail::operation(&nd_complete_op::do_complete)
      , op_(verbs_op){
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base_op,
                          [[maybe_unused]]asio::error_code const& ec,
                          [[maybe_unused]]std::size_t bytes_transferred) {
    nd_complete_op* o = static_cast<nd_complete_op*>(base_op);
    auto* op = o->op_;
    assert(op);

    // ptr object for destruct and free operation
    ptr p = {nullptr, o, o};
    p.reset();

    // callback
    op->complete(owner);
  }
};

}