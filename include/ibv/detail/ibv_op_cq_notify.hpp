#pragma once

#include <array>

#include "asio/detail/op_queue.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/reactor_op.hpp"
#include "asio/error.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

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

// Bridges libibverbs CQ completions into the epoll reactor. Armed (per posted
// verbs op) on the shared comp_channel fd. As an asio reactor_op it returns
// not_done to stay armed until a completion is available, then done to dispatch.
// Armed with allow_speculative so a completion that landed before arming is
// caught by the immediate poll (avoids the post-before-notify race on fast
// loopback). Mirrors nd_notify_wr_op + deprecated rdma_cq_notify_op.
class ibv_cq_notify_op : public asio::detail::reactor_op {
public:
  static constexpr int poll_batch = 16;

  ibv_cq_notify_op(asio::error_code const& success_ec,
                   native_comp_channel_t* comp_channel, native_cq_t* cq)
      : asio::detail::reactor_op(success_ec, &do_perform, &do_complete)
      , comp_channel_(comp_channel)
      , cq_(cq) {
  }

  // req_notify_cq then arm a poller on the comp_channel fd (speculative so an
  // already-present completion is drained immediately).
  static void arm(asio::error_code const& success_ec,
                  asio::detail::reactor& reactor,
                  asio::detail::reactor::per_descriptor_data& descriptor_data,
                  native_comp_channel_t* comp_channel, native_cq_t* cq) {
    asio::error_code ec;
    verbs_ops::req_notify_cq(cq, false, ec);
    auto* op = new ibv_cq_notify_op(success_ec, comp_channel, cq);
    reactor.start_op(asio::detail::reactor::read_op, comp_channel->fd,
                     descriptor_data, op, false, true);
  }

private:
  static rdma_verbs_op_base* resolve(native_wc_t const& wc) {
    if (!wc.wr_id) {
      return nullptr;
    }
    auto* op = reinterpret_cast<rdma_verbs_op_base*>(wc.wr_id);
    if (wc.status == IBV_WC_SUCCESS) {
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

  static status do_perform(asio::detail::reactor_op* base) {
    auto* o = static_cast<ibv_cq_notify_op*>(base);

    // Consume a comp_channel event if one is queued, and re-arm CQ notification
    // for the next one. fd is O_NONBLOCK so this is EAGAIN when none.
    native_cq_t* ev_cq = nullptr;
    void* ev_ctx = nullptr;
    if (verbs_ops::get_cq_event(o->comp_channel_, &ev_cq, &ev_ctx) == 0) {
      verbs_ops::ack_cq_events(ev_cq, 1);
      asio::error_code ec;
      verbs_ops::req_notify_cq(ev_cq, false, ec);
    }

    // Drain the CQ directly (independent of the event), collecting ops.
    int n = 0;
    do {
      std::array<native_wc_t, poll_batch> wcs{};
      n = verbs_ops::poll_cq(o->cq_, poll_batch, wcs.data());
      for (int i = 0; i < n; ++i) {
        if (auto* op = resolve(wcs[i])) {
          o->completed_.push(op);
        }
      }
    } while (n > 0);

    // No completion yet: stay armed on the fd. Otherwise retire and dispatch.
    return o->completed_.empty() ? not_done : done;
  }

  static void do_complete(void* owner, asio::detail::operation* base,
                          asio::error_code const& /*result_ec*/,
                          std::size_t /*bytes_transferred*/) {
    auto* o = static_cast<ibv_cq_notify_op*>(base);
    // Drain the collected verbs ops before freeing this op. owner == nullptr
    // (reactor shutdown) => free handlers without an upcall.
    if (owner) {
      while (auto* wc_op = o->completed_.front()) {
        o->completed_.pop();
        wc_op->complete(owner);
      }
    }
    else {
      while (auto* wc_op = o->completed_.front()) {
        o->completed_.pop();
        wc_op->destroy();
      }
    }
    delete o;
  }

  native_comp_channel_t* comp_channel_;
  native_cq_t* cq_;
  asio::detail::op_queue<rdma_verbs_op_base> completed_;
};

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
