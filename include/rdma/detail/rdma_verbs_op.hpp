#pragma once

#include <cassert>
#include <cstddef>

#include "asio/detail/op_queue.hpp"
#include "asio/error.hpp"
#include "rdma/rdma_buffer.hpp"
#include "rdma/rdma_commons.hpp"

// Backend-independent data-plane op hierarchy shared by nd and ibv. These types
// hold no native handles: completion is dispatched by each backend's CQ poller
// (via ND2 RequestContext / ibv wr_id) which casts back to rdma_verbs_op_base*
// and calls complete().
namespace asio::rdma::detail {

class rdma_verbs_op_base {
public:
  friend class asio::detail::op_queue_access;

  using complete_func =
      void (*)(void*, rdma_verbs_op_base*, asio::error_code const&, std::size_t);

  enum class op_type {
    post_recv = 0,
    post_send,
    remote_read,
    remote_write,
    max_ops,
  };

protected:
  complete_func complete_func_;
  rdma_verbs_op_base* next_;

public:
  asio::error_code ec_;
  std::size_t bytes_transferred_;
  void* cancellation_key_;

  rdma_verbs_op_base(complete_func complete_cb,
                     asio::error_code const& success_ec)
      : complete_func_(complete_cb)
      , next_(nullptr)
      , ec_(success_ec)
      , bytes_transferred_(0)
      , cancellation_key_(nullptr) {
    assert(complete_func_);
  }

  void complete(void* owner) {
    assert(complete_func_);
    complete_func_(owner, this, ec_, bytes_transferred_);
  }

  void destroy() {
    assert(complete_func_);
    complete_func_(nullptr, this, asio::error_code{}, 0);
  }
};

template <typename BufferSequence>
struct rdma_two_sided_op;
template <typename BufferSequence>
struct rdma_one_sided_op;

template <mr_adapted_buffer_sequence BufferSequence>
class rdma_two_sided_op<BufferSequence> : public rdma_verbs_op_base {
public:
  using complete_func = rdma_verbs_op_base::complete_func;
  using op_type = rdma_verbs_op_base::op_type;

protected:
  BufferSequence buffer_seq_;

public:
  rdma_two_sided_op(complete_func complete_cb,
                    asio::error_code const& success_ec,
                    BufferSequence const& buffer_seq)
      : rdma_verbs_op_base(complete_cb, success_ec)
      , buffer_seq_(buffer_seq) {
  }

  BufferSequence const& get_buffer_sequence() const noexcept {
    return buffer_seq_;
  }

  void set_posted_bytes(std::size_t bytes) noexcept {
    bytes_transferred_ = bytes;
  }
};

template <typename BufferSequence>
class rdma_one_sided_op : public rdma_two_sided_op<BufferSequence> {
public:
  using base_type = rdma_two_sided_op<BufferSequence>;
  using complete_func = typename base_type::complete_func;
  using op_type = typename base_type::op_type;

protected:
  rdma_remote_addr_t remote_addr_;

public:
  rdma_one_sided_op(complete_func complete_cb,
                    asio::error_code const& success_ec,
                    BufferSequence const& buffer_seq,
                    rdma_remote_addr_t const& remote_addr)
      : base_type(complete_cb, success_ec, buffer_seq)
      , remote_addr_(remote_addr) {
  }

  rdma_remote_addr_t const& get_remote_addr() const noexcept {
    return remote_addr_;
  }
};

}
