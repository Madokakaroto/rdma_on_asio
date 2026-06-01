#pragma once

#include "asio/detail/operation.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"
#include "nd/nd_buffer.hpp"
#include "rdma/detail/rdma_verbs_op.hpp"

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

// The verbs op base hierarchy is backend-independent
// (rdma/detail/rdma_verbs_op.hpp); keep nd-prefixed aliases.
using nd_verbs_op_base = rdma_verbs_op_base;

template <typename BufferSequence>
using nd_two_sided_op = rdma_two_sided_op<BufferSequence>;

template <typename BufferSequence>
using nd_one_sided_op = rdma_one_sided_op<BufferSequence>;

}