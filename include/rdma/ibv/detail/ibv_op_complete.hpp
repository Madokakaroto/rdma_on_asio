#pragma once

#include <infiniband/verbs.h>

#include "asio/detail/operation.hpp"
#include "asio/error.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

// Map an ibv_wc_status to an asio error_code. Success clears; a flush (posted
// WRs drained on disconnect) maps to operation_aborted; everything else to a
// connection failure.
inline asio::error_code wc_status_to_ec(ibv_wc_status status) {
  switch (status) {
    case IBV_WC_SUCCESS:
      return asio::error_code{};
    case IBV_WC_WR_FLUSH_ERR:
      return asio::error::operation_aborted;
    default:
      return asio::error::connection_reset;
  }
}

// Resolve a work completion back to its originating verbs op (wr_id is the op
// pointer) and stamp ec_ / bytes_transferred_. Shared by the CQ poller (event
// mode) and ibv_completion_queue::poll (poll mode).
inline rdma_verbs_op_base* resolve_verbs_op(native_wc_t const& wc) {
  if (!wc.wr_id) {
    return nullptr;
  }
  auto* op = reinterpret_cast<rdma_verbs_op_base*>(wc.wr_id);
  if (wc.status == IBV_WC_SUCCESS) {
    // send/write don't report byte_len; recv/read do.
    if (wc.opcode != IBV_WC_SEND && wc.opcode != IBV_WC_RDMA_WRITE) {
      op->bytes_transferred_ = wc.byte_len;
    }
  }
  else {
    op->bytes_transferred_ = 0;
    op->ec_ = wc_status_to_ec(wc.status);
  }
  return op;
}

// Wraps a verbs op for immediate completion via the scheduler (sync post error
// path). Mirrors nd_complete_op.
class ibv_complete_op : public asio::detail::operation {
public:
  explicit ibv_complete_op(rdma_verbs_op_base* op)
      : asio::detail::operation(&do_complete)
      , op_(op) {
  }

private:
  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*ec*/,
                          std::size_t /*bytes*/) {
    auto* o = static_cast<ibv_complete_op*>(base);
    rdma_verbs_op_base* verbs_op = o->op_;
    delete o;
    verbs_op->complete(owner);
  }

  rdma_verbs_op_base* op_;
};

}
