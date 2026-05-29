#pragma once

#include "asio/detail/operation.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"
#include "nd/nd_buffer.hpp"

namespace asio::rdma::detail {

class nd_op_base : public asio::detail::operation {
public:
  enum class status_t {
    completed,
    continuation,
  };
  using process_func = status_t(*)(void* owner, nd_op_base* base, asio::error_code& ec);

private:
  IND2Overlapped* overlapped_;
  process_func process_func_;

protected:
  nd_op_base(IND2Overlapped* overlapped, process_func perform_function,
             func_type complete_func)
      : asio::detail::operation(complete_func)
      , overlapped_(overlapped)
      , process_func_(perform_function) {
  }

  IND2Overlapped* get_overlapped() const { return overlapped_; }

  static status_t default_process(void* owner, nd_op_base* base,
                                  asio::error_code& ec) {
    return status_t::completed;
  }

  status_t resume_process(void* owner, asio::error_code& ec) {
    if (ec) {
      return status_t::completed;
    }
    auto const status = do_process(owner, ec);
    if (status_t::continuation == status) {
      assert(owner);
      auto* context = static_cast<asio::detail::win_iocp_io_context*>(owner);
      context->work_started();
    }
    return status;
  }

private:
  status_t do_process(void* owner, asio::error_code& ec) {
    assert(overlapped_);

    auto const hr = this->overlapped_->GetOverlappedResult(this, false);
    if (hr != ND_SUCCESS) {
      ec = static_cast<nd_errc>(hr);
      return status_t::completed;
    }

    if (process_func_) {
      return process_func_(owner, this, ec);
    }
    return status_t::completed;
  }
};

class nd_verbs_op_base {
public:
  // to use op_queue
  friend class asio::detail::op_queue_access;
  // the io complete function
  using complete_func = void(*)(void*, nd_verbs_op_base*, asio::error_code const&, std::size_t);
  // type of io operation
  enum class op_type {
    post_recv = 0,
    post_send,
    remote_read,
    remote_write,
    max_ops,
  };

 protected:
  // callback on io completion
  complete_func complete_func_;

  // link list node
  nd_verbs_op_base* next_;

public:
  // The error code to be passed to the completion handler.
  asio::error_code ec_;

  // The number of bytes transferred, to be passed to the completion handler.
  std::size_t bytes_transferred_;

  // The rdma operation key used for targeted cancellation.
  void* cancellation_key_;

public:
  nd_verbs_op_base(complete_func complete_cb,
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
    complete_func_(0, this, asio::error_code{}, 0); 
  }
};

template <typename BufferSequence>
struct nd_two_sided_op;
template <typename BufferSeuqence>
struct nd_one_sided_op;

template <mr_adapted_buffer_sequence BufferSequence>
class nd_two_sided_op<BufferSequence> : public nd_verbs_op_base {
 public:
  using complete_func = nd_verbs_op_base::complete_func;
  using op_type = nd_verbs_op_base::op_type;

 protected:
  BufferSequence buffer_seq_;

 public:
  nd_two_sided_op(complete_func complete_cb,
                  asio::error_code const& success_ec,
                  BufferSequence const& buffer_seq)
      : nd_verbs_op_base(complete_cb, success_ec)
      , buffer_seq_(buffer_seq) {
  }

  BufferSequence const& get_buffer_sequence() const noexcept {
    return buffer_seq_;
  }
};

template <typename BufferSequence>
class nd_one_sided_op : public nd_two_sided_op<BufferSequence> {
 public:
  using base_type = nd_two_sided_op<BufferSequence>;
  using complete_func = typename base_type::complete_func;
  using op_type = typename base_type::op_type;

 protected:
  BufferSequence buffer_seq_;
  nd_remote_addr_t remote_addr_;

 public:
  nd_one_sided_op(complete_func complete_cb,
                  asio::error_code const& success_ec,
                  BufferSequence const& buffer_seq,
                  nd_remote_addr_t const& remote_addr) 
    : base_type(complete_cb, success_ec, buffer_seq)
    , buffer_seq_(buffer_seq)
    , remote_addr_(remote_addr){
  }

  nd_remote_addr_t const& get_remote_addr() const noexcept {
    return remote_addr_;
  }
};

}