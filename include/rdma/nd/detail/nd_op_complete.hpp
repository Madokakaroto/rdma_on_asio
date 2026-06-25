#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/nd/detail/nd_op_base.hpp"

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
  ASIO_DECL static void do_complete(void* owner, asio::detail::operation* base_op,
                                    asio::error_code const& ec,
                                    std::size_t bytes_transferred);
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_op_complete.ipp"
#endif