#pragma once

#include <infiniband/verbs.h>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/operation.hpp"
#include "asio/error.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Map an ibv_wc_status to an asio error_code. Success clears; a flush (posted
// WRs drained on disconnect) maps to operation_aborted; everything else to a
// connection failure.
ASIO_DECL asio::error_code wc_status_to_ec(ibv_wc_status status);

// Resolve a work completion back to its originating verbs op (wr_id is the op
// pointer) and stamp ec_ / bytes_transferred_. Shared by the CQ poller (event
// mode) and ibv_completion_queue::poll (poll mode).
ASIO_DECL rdma_verbs_op_base* resolve_verbs_op(native_wc_t const& wc);

// Wraps a verbs op for immediate completion via the scheduler (sync post error
// path). Mirrors nd_complete_op.
class ibv_complete_op : public asio::detail::operation {
public:
  explicit ibv_complete_op(rdma_verbs_op_base* op)
      : asio::detail::operation(&do_complete)
      , op_(op) {
  }

private:
  ASIO_DECL static void do_complete(void* owner, asio::detail::operation* base,
                                    asio::error_code const& ec,
                                    std::size_t bytes);

  rdma_verbs_op_base* op_;
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_op_complete.ipp"
#endif
