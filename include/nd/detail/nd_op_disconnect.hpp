#pragma once

#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

// Fire-and-forget disconnect op (no handler): backs the synchronous,
// non-blocking connector::disconnect(). ND2 Disconnect is overlapped, so we
// issue it with this self-reaping op and return without waiting; when IOCP
// completes the overlapped, do_complete simply frees the op (no user upcall).
// Mirrors nd_poll_wc_op's self-perpetuating/self-reaping pattern.
class nd_disconnect_op final : public nd_op_base {
public:
  struct Handler {};
  ASIO_DEFINE_HANDLER_PTR(nd_disconnect_op);

  explicit nd_disconnect_op(IND2Connector* connector)
      : nd_op_base(connector, &nd_op_base::default_process,
                   &nd_disconnect_op::do_complete) {
  }

private:
  static void do_complete(void* /*owner*/, asio::detail::operation* base,
                          const asio::error_code& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    auto* o = static_cast<nd_disconnect_op*>(base);
    ptr p = {nullptr, o, o};
    p.reset();  // fire-and-forget: free the op, no upcall
  }
};

}

#include "asio/detail/pop_options.hpp"
