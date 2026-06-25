#ifndef RDMA_ND_DETAIL_IMPL_ND_OP_DISCONNECT_IPP
#define RDMA_ND_DETAIL_IMPL_ND_OP_DISCONNECT_IPP

#include "rdma/nd/detail/nd_op_disconnect.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

nd_disconnect_op::nd_disconnect_op(IND2Connector* connector)
    : nd_op_base(connector, &nd_op_base::default_process,
                 &nd_disconnect_op::do_complete) {
}

void nd_disconnect_op::do_complete(void* /*owner*/,
                                   asio::detail::operation* base,
                                   const asio::error_code& /*result_ec*/,
                                   std::size_t /*bytes_transferred*/) {
  auto* o = static_cast<nd_disconnect_op*>(base);
  ptr p = {nullptr, o, o};
  p.reset();  // fire-and-forget: free the op, no upcall
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_OP_DISCONNECT_IPP
