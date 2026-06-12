#include "unit_test.hpp"

#include <concepts>

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

void ibv_public_headers_compile()
{
}

void ibv_listener_bind_is_port_only()
{
  using listener = rdma::ibv_listener<rdma::tcp>;
  using endpoint = rdma::tcp::endpoint;

  static_assert(can_bind<listener, asio::ip::port_type>);
  static_assert(can_bind_with_ec<listener, asio::ip::port_type>);
  static_assert(!can_bind<listener, endpoint>);
  static_assert(!can_bind_with_ec<listener, endpoint>);
}

ASIO_TEST_SUITE
(
  "ibv/public_headers",
  ASIO_COMPILE_TEST_CASE(ibv_public_headers_compile)
  ASIO_COMPILE_TEST_CASE(ibv_listener_bind_is_port_only)
)
