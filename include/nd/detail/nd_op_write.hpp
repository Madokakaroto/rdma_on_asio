#pragma once

#include "rdma/detail/rdma_op_write.hpp"

namespace asio::rdma::detail {

template <mr_const_buffer_sequence BufferSequence, typename Handler,
          typename IoExecutor>
using nd_write_op = rdma_write_op<BufferSequence, Handler, IoExecutor>;

}
