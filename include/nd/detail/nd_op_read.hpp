#pragma once

#include "rdma/detail/rdma_op_read.hpp"

namespace asio::rdma::detail {

template <mr_mutable_buffer_sequence BufferSequence, typename Handler,
          typename IoExecutor>
using nd_read_op = rdma_read_op<BufferSequence, Handler, IoExecutor>;

}
