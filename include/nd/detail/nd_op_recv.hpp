#pragma once

#include "rdma/detail/rdma_op_recv.hpp"

namespace asio::rdma::detail {

template <mr_mutable_buffer_sequence BufferSequence, typename Handler,
          typename IoExecutor>
using nd_recv_op = rdma_recv_op<BufferSequence, Handler, IoExecutor>;

}
