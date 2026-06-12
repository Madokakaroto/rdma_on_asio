#include <system_error>
#include <utility>

#include "unit_test.hpp"

#include "asio/buffer.hpp"
#include "asio/io_context.hpp"
#include "rdma/ibv/ibv_connector.hpp"
#include "rdma/ibv/ibv_listener.hpp"
#include "rdma/ibv/ibv_mr.hpp"
#include "rdma/ibv/ibv_queue_pair.hpp"
#include "rdma/ibv/ibv_use_device.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;

void open_and_bind_without_use_device()
{
  asio::io_context io;
  asio::error_code ec;

  rdma::ibv_connector<rdma::tcp> connector(io);
  connector.open(rdma::tcp::v4(), ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!connector.is_open());

  rdma::ibv_listener<rdma::tcp> listener(io);
  listener.open(rdma::tcp::v4(), ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!listener.is_open());

  rdma::ibv_queue_pair qp;
  qp.bind(io, ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!qp.is_bound());
  ASIO_CHECK(qp.bound_type() == rdma::completion_mode::none);
}

void default_queue_pair_state()
{
  rdma::ibv_queue_pair qp;

  ASIO_CHECK(!qp.is_bound());
  ASIO_CHECK(qp.bound_type() == rdma::completion_mode::none);
  ASIO_CHECK(qp.native_handle() == nullptr);
}

void invalid_assign_handle()
{
  asio::io_context io;
  asio::error_code ec;
  rdma::ibv_connector<rdma::tcp> connector(io);
  rdma::detail::ibv_connector_handle_t handle;

  connector.assign(std::move(handle), ec);

  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);
  ASIO_CHECK(!connector.is_open());
}

void async_connect_without_use_device_completes_device_not_registered()
{
  asio::io_context io;
  rdma::ibv_connector<rdma::tcp> connector(io);
  rdma::ibv_queue_pair qp;
  auto endpoint = rdma::tcp::v4().any_endpoint(0);

  bool called = false;
  asio::error_code got;
  std::size_t reply_len = 99;
  connector.async_connect(
      qp, endpoint, asio::const_buffer{}, asio::mutable_buffer{},
      [&](asio::error_code ec, std::size_t n) {
        called = true;
        got = ec;
        reply_len = n;
      });

  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(called);
  ASIO_CHECK(got == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(reply_len == 0);
}

void async_accept_without_use_device_completes_device_not_registered()
{
  asio::io_context io;
  rdma::ibv_connector<rdma::tcp> connector(io);
  rdma::ibv_queue_pair qp;

  bool called = false;
  asio::error_code got;
  connector.async_accept(qp, asio::const_buffer{},
                         [&](asio::error_code ec) {
                           called = true;
                           got = ec;
                         });

  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(called);
  ASIO_CHECK(got == rdma::rdma_errc::device_not_registered);
}

void use_device_null_device()
{
  asio::io_context io;
  asio::error_code ec;

  rdma::use_device(io, rdma::ibv_device_ptr{}, rdma::ibv_config_t{}, ec);

  ASIO_CHECK(ec == rdma::rdma_errc::invalid_device);
}

void memory_region_null_device_throws()
{
  unsigned char storage[8] = {};

  try
  {
    rdma::ibv_memory_region mr(rdma::ibv_device_ptr{}, storage,
                               sizeof(storage));
    (void)mr;
    ASIO_ERROR("ibv_memory_region accepted a null device");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::invalid_device);
  }
}

ASIO_TEST_SUITE
(
  "ibv/service_state_guards",
  ASIO_TEST_CASE(open_and_bind_without_use_device)
  ASIO_TEST_CASE(default_queue_pair_state)
  ASIO_TEST_CASE(invalid_assign_handle)
  ASIO_TEST_CASE(async_connect_without_use_device_completes_device_not_registered)
  ASIO_TEST_CASE(async_accept_without_use_device_completes_device_not_registered)
  ASIO_TEST_CASE(use_device_null_device)
  ASIO_TEST_CASE(memory_region_null_device_throws)
)
