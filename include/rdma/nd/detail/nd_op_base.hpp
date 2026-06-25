#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/operation.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/nd_buffer.hpp"
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

  ASIO_DECL status_t resume_process(void* owner, asio::error_code& ec);

private:
  ASIO_DECL status_t do_process(void* owner, asio::error_code& ec);
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_op_base.ipp"
#endif
