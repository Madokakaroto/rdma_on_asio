#include "unit_test.hpp"

#include <concepts>

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
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;

template <typename Listener, typename Arg>
concept can_bind = requires(Listener& listener, Arg arg) {
  listener.bind(arg);
};

template <typename Listener, typename Arg>
concept can_bind_with_ec = requires(Listener& listener, Arg arg,
                                    asio::error_code& ec) {
  listener.bind(arg, ec);
};

void nd_public_headers_compile()
{
}

void nd_listener_bind_is_port_only()
{
  using listener = rdma::nd_listener<rdma::tcp>;
  using endpoint = rdma::tcp::endpoint;

  static_assert(can_bind<listener, asio::ip::port_type>);
  static_assert(can_bind_with_ec<listener, asio::ip::port_type>);
  static_assert(!can_bind<listener, endpoint>);
  static_assert(!can_bind_with_ec<listener, endpoint>);
}

ASIO_TEST_SUITE
(
  "nd/public_headers",
  ASIO_COMPILE_TEST_CASE(nd_public_headers_compile)
  ASIO_COMPILE_TEST_CASE(nd_listener_bind_is_port_only)
)
