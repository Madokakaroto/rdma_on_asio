#ifndef RDMA_ND_IMPL_ND_OP_COMPLETE_IPP
#define RDMA_ND_IMPL_ND_OP_COMPLETE_IPP

#include <cassert>

#include "rdma/nd/detail/nd_op_complete.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

void nd_complete_op::do_complete(void* owner, asio::detail::operation* base_op,
                                 asio::error_code const& /*ec*/,
                                 std::size_t /*bytes_transferred*/) {
  nd_complete_op* o = static_cast<nd_complete_op*>(base_op);
  auto* op = o->op_;
  assert(op);

  // ptr object for destruct and free operation
  ptr p = {nullptr, o, o};
  p.reset();

  // callback
  op->complete(owner);
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_IMPL_ND_OP_COMPLETE_IPP
