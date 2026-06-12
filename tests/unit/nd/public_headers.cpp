#include "unit_test.hpp"

#include <WinSock2.h>
#include <ws2tcpip.h>

#include "rdma/nd/nd_buffer.hpp"
#include "rdma/nd/nd_completion_queue.hpp"
#include "rdma/nd/nd_connector.hpp"
#include "rdma/nd/nd_device.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/nd_listener.hpp"
#include "rdma/nd/nd_mr.hpp"
#include "rdma/nd/nd_queue_pair.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_use_device.hpp"

void nd_public_headers_compile()
{
}

ASIO_TEST_SUITE
(
  "nd/public_headers",
  ASIO_COMPILE_TEST_CASE(nd_public_headers_compile)
)
