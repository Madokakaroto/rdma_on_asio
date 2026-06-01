#pragma once

#include "rdma/detail/rdma_op_send.hpp"

namespace asio::rdma::detail {

template <mr_const_buffer_sequence BufferSequence, typename Handler,
          typename IoExecutor>
using nd_send_op = rdma_send_op<BufferSequence, Handler, IoExecutor>;

}
