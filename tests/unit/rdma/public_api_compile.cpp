#include <array>
#include <type_traits>

#include "unit_test.hpp"

#include "asio/deferred.hpp"
#include "asio/io_context.hpp"
#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;

void umbrella_include_and_aliases_compile()
{
  using connector = rdma::rdma_connector<rdma::tcp>;
  using listener = rdma::rdma_listener<rdma::tcp>;
  using queue_pair = rdma::rdma_queue_pair;
  using completion_queue = rdma::rdma_completion_queue;
  using memory_region = rdma::rdma_memory_region;
  using device = rdma::rdma_device;
  using device_ptr = rdma::rdma_device_ptr;

  static_assert(!std::is_copy_constructible_v<connector>);
  static_assert(!std::is_copy_constructible_v<listener>);
  static_assert(!std::is_copy_constructible_v<queue_pair>);
  static_assert(std::is_move_constructible_v<connector>);
  static_assert(std::is_move_constructible_v<listener>);
  static_assert(std::is_move_constructible_v<queue_pair>);

  (void)sizeof(completion_queue);
  (void)sizeof(memory_region);
  (void)sizeof(device);
  (void)sizeof(device_ptr);
}

void async_overloads_compile()
{
  asio::io_context io;
  rdma::rdma_connector<rdma::tcp> connector(io);
  rdma::rdma_listener<rdma::tcp> listener(io);
  rdma::rdma_queue_pair qp;
  auto endpoint = rdma::tcp::v4().any_endpoint(0);

  std::array<rdma::const_buffer, 1> send_buffers{};
  std::array<rdma::mutable_buffer, 1> recv_buffers{};
  rdma::rdma_remote_addr_t remote{};

  auto send_op = qp.async_send(send_buffers, asio::deferred);
  auto recv_op = qp.async_recv(recv_buffers, asio::deferred);
  auto write_op = qp.async_write(
      send_buffers, rdma::rdma_remote_addr_t{remote.addr_, remote.token_},
      asio::deferred);
  auto read_op = qp.async_read(
      recv_buffers, rdma::rdma_remote_addr_t{remote.addr_, remote.token_},
      asio::deferred);

  auto connect_with_reply =
      connector.async_connect(qp, endpoint, asio::const_buffer{},
                              asio::mutable_buffer{}, asio::deferred);
  auto connect_no_reply =
      connector.async_connect(qp, endpoint, asio::const_buffer{},
                              asio::deferred);
  auto accept_with_data =
      connector.async_accept(qp, asio::const_buffer{}, asio::deferred);
  auto accept_no_data = connector.async_accept(qp, asio::deferred);
  auto move_accept_with_data =
      connector.async_accept(io, asio::const_buffer{}, asio::deferred);
  auto move_accept_no_data = connector.async_accept(io, asio::deferred);
  auto wait_disconnect = connector.async_wait_disconnect(asio::deferred);

  auto get_connection_return =
      listener.async_get_connection(asio::mutable_buffer{}, asio::deferred);
  auto get_connection_fill =
      listener.async_get_connection(connector, asio::mutable_buffer{},
                                    asio::deferred);

  (void)send_op;
  (void)recv_op;
  (void)write_op;
  (void)read_op;
  (void)connect_with_reply;
  (void)connect_no_reply;
  (void)accept_with_data;
  (void)accept_no_data;
  (void)move_accept_with_data;
  (void)move_accept_no_data;
  (void)wait_disconnect;
  (void)get_connection_return;
  (void)get_connection_fill;
}

ASIO_TEST_SUITE
(
  "rdma/public_api_compile",
  ASIO_COMPILE_TEST_CASE(umbrella_include_and_aliases_compile)
  ASIO_COMPILE_TEST_CASE(async_overloads_compile)
)
