#include "unit_test.hpp"

#include "rdma/ibv/ibv_buffer.hpp"
#include "rdma/ibv/ibv_completion_queue.hpp"
#include "rdma/ibv/ibv_connector.hpp"
#include "rdma/ibv/ibv_device.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_listener.hpp"
#include "rdma/ibv/ibv_mr.hpp"
#include "rdma/ibv/ibv_queue_pair.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/ibv_use_device.hpp"

void ibv_public_headers_compile()
{
}

ASIO_TEST_SUITE
(
  "ibv/public_headers",
  ASIO_COMPILE_TEST_CASE(ibv_public_headers_compile)
)
