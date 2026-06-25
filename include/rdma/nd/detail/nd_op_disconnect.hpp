#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/memory.hpp"
#include "rdma/nd/detail/nd_op_base.hpp"
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

  ASIO_DECL explicit nd_disconnect_op(IND2Connector* connector);

private:
  ASIO_DECL static void do_complete(void* /*owner*/,
                                    asio::detail::operation* base,
                                    const asio::error_code& /*result_ec*/,
                                    std::size_t /*bytes_transferred*/);
};

}

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_op_disconnect.ipp"
#endif
