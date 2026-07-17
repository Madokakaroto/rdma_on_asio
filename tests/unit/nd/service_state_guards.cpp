#include <array>
#include <atomic>
#include <cstdint>
#include <system_error>
#include <utility>

#include "unit_test.hpp"

#include "asio/buffer.hpp"
#include "asio/io_context.hpp"
#include "asio/system_executor.hpp"
#include "rdma/nd/nd_connector.hpp"
#include "rdma/nd/nd_listener.hpp"
#include "rdma/nd/nd_mr.hpp"
#include "rdma/nd/nd_queue_pair.hpp"
#include "rdma/nd/nd_use_device.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;

void open_and_bind_without_use_device()
{
  asio::io_context io;
  asio::error_code ec;

  rdma::nd_connector<rdma::tcp> connector(io);
  connector.open(rdma::tcp::v4(), ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!connector.is_open());

  rdma::nd_listener<rdma::tcp> listener(io);
  listener.open(rdma::tcp::v4(), ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!listener.is_open());

  rdma::nd_queue_pair qp;
  qp.bind(io, ec);
  ASIO_CHECK(ec == rdma::rdma_errc::device_not_registered);
  ASIO_CHECK(!qp.is_bound());
  ASIO_CHECK(qp.bound_type() == rdma::completion_mode::none);
}

void throwing_overloads_without_use_device()
{
  asio::io_context io;

  try
  {
    rdma::nd_connector<rdma::tcp> connector(io, rdma::tcp::v4());
    (void)connector;
    ASIO_ERROR("nd_connector opening constructor accepted missing use_device");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::device_not_registered);
  }

  try
  {
    rdma::nd_listener<rdma::tcp> listener(io);
    listener.open(rdma::tcp::v4());
    ASIO_ERROR("nd_listener::open accepted missing use_device");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::device_not_registered);
  }

  try
  {
    rdma::nd_queue_pair qp;
    qp.bind(io);
    ASIO_ERROR("nd_queue_pair::bind accepted missing use_device");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::device_not_registered);
  }

  try
  {
    rdma::use_device(io, rdma::nd_device_ptr{});
    ASIO_ERROR("use_device accepted a null nd_device_ptr");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::invalid_device);
  }
}

void default_queue_pair_state()
{
  rdma::nd_queue_pair qp;

  ASIO_CHECK(!qp.is_bound());
  ASIO_CHECK(qp.bound_type() == rdma::completion_mode::none);
  ASIO_CHECK(qp.native_handle() == nullptr);
}

void invalid_windows_handles_are_not_closable()
{
  ASIO_CHECK(!rdma::detail::is_closable_handle(nullptr));
  ASIO_CHECK(!rdma::detail::is_closable_handle(INVALID_HANDLE_VALUE));
  ASIO_CHECK(rdma::detail::is_closable_handle(
      reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1))));
}

void create_overlapped_file_checked_has_strict_failure_contract()
{
  asio::error_code ec;
  auto handle = rdma::detail::create_overlapped_file_checked(
      [](HANDLE*) { return E_FAIL; }, ec);
  ASIO_CHECK(handle == nullptr);
  ASIO_CHECK(ec == rdma::make_nd_error_code(E_FAIL));

  handle = rdma::detail::create_overlapped_file_checked(
      [](HANDLE* out) {
        *out = INVALID_HANDLE_VALUE;
        return E_FAIL;
      }, ec);
  ASIO_CHECK(handle == nullptr);
  ASIO_CHECK(ec == rdma::make_nd_error_code(E_FAIL));

  handle = rdma::detail::create_overlapped_file_checked(
      [](HANDLE* out) {
        *out = INVALID_HANDLE_VALUE;
        return S_OK;
      }, ec);
  ASIO_CHECK(handle == nullptr);
  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);

  auto expected = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1));
  handle = rdma::detail::create_overlapped_file_checked(
      [expected](HANDLE* out) {
        *out = expected;
        return S_OK;
      }, ec);
  ASIO_CHECK(handle == expected);
  ASIO_CHECK(!ec);
}

void invalid_assign_handle()
{
  asio::io_context io;
  asio::error_code ec;
  rdma::nd_connector<rdma::tcp> connector(io);
  rdma::detail::nd_connector_handle_t handle;

  connector.assign(std::move(handle), ec);

  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);
  ASIO_CHECK(!connector.is_open());
}

void listener_unopened_guards()
{
  asio::io_context io;
  asio::error_code ec;
  rdma::nd_listener<rdma::tcp> listener(io);

  listener.bind(0, ec);
  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);

  listener.listen(1, ec);
  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);
}

void connector_unopened_disconnect_guards()
{
  asio::io_context io;
  asio::error_code ec;
  rdma::nd_connector<rdma::tcp> connector(io);

  connector.disconnect(ec);
  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);

  bool called = false;
  asio::error_code got;
  connector.async_wait_disconnect([&](asio::error_code wait_ec) {
    called = true;
    got = wait_ec;
  });

  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(called);
  ASIO_CHECK(got == rdma::rdma_errc::invalid_handle);
}

void listener_unopened_async_get_connection_guards()
{
  asio::io_context io;
  rdma::nd_listener<rdma::tcp> listener(io);
  std::array<unsigned char, 8> request{};

  bool return_called = false;
  asio::error_code return_ec;
  std::size_t return_len = 99;
  bool returned_connector_open = true;
  listener.async_get_connection(
      asio::buffer(request),
      [&](asio::error_code ec, rdma::nd_connector<rdma::tcp> conn,
          std::size_t n) {
        return_called = true;
        return_ec = ec;
        return_len = n;
        returned_connector_open = conn.is_open();
      });

  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(return_called);
  ASIO_CHECK(return_ec == rdma::rdma_errc::invalid_handle);
  ASIO_CHECK(return_len == 0);
  ASIO_CHECK(!returned_connector_open);

  io.restart();
  rdma::nd_connector<rdma::tcp> conn(io);
  bool fill_called = false;
  asio::error_code fill_ec;
  std::size_t fill_len = 99;
  listener.async_get_connection(
      conn, asio::buffer(request),
      [&](asio::error_code ec, std::size_t n) {
        fill_called = true;
        fill_ec = ec;
        fill_len = n;
      });

  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(fill_called);
  ASIO_CHECK(fill_ec == rdma::rdma_errc::invalid_handle);
  ASIO_CHECK(fill_len == 0);
  ASIO_CHECK(!conn.is_open());
}

void async_connect_without_use_device_completes_device_not_registered()
{
  asio::io_context io;
  rdma::nd_connector<rdma::tcp> connector(io);
  rdma::nd_queue_pair qp;
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
  rdma::nd_connector<rdma::tcp> connector(io);
  rdma::nd_queue_pair qp;

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

void move_accept_without_use_device_returns_empty_queue_pair()
{
  asio::io_context io;
  rdma::nd_connector<rdma::tcp> connector(io);
  bool called = false;
  bool returned_inline = true;
  asio::error_code got;

  connector.async_accept(
      io, asio::const_buffer{},
      [&](asio::error_code ec, rdma::nd_queue_pair qp) {
        called = true;
        got = ec;
        ASIO_CHECK(!returned_inline);
        ASIO_CHECK(!qp.is_bound());
        ASIO_CHECK(qp.native_handle() == nullptr);
      });
  returned_inline = false;

  ASIO_CHECK(!called);
  ASIO_CHECK(io.run() == 1);
  ASIO_CHECK(called);
  ASIO_CHECK(got == rdma::rdma_errc::device_not_registered);
}

void started_accept_failure_makes_connector_terminal()
{
  std::atomic<rdma::detail::connect_state> state{
      rdma::detail::connect_state::connecting};
  bool called = false;
  auto handler = [&](asio::error_code ec) {
    called = true;
    ASIO_CHECK(ec == rdma::nd_errc::connection_aborted);
  };
  using handler_type = decltype(handler);
  using op_type = rdma::detail::nd_accept_op<handler_type,
                                               asio::system_executor>;
  typename op_type::ptr p = {asio::detail::addressof(handler),
                             op_type::ptr::allocate(handler), 0};
  p.p = new (p.v) op_type{nullptr, &state, handler,
                          asio::system_executor{}};
  auto* op = p.p;
  op->mark_started();
  p.v = p.p = nullptr;

  int owner = 0;
  op->complete(&owner, make_error_code(rdma::nd_errc::connection_aborted), 0);

  ASIO_CHECK(called);
  ASIO_CHECK(state.load() == rdma::detail::connect_state::closed);
}

void connect_op_shutdown_does_not_dereference_connector()
{
  std::atomic<rdma::detail::connect_state> state{
      rdma::detail::connect_state::connecting};
  bool called = false;
  auto handler = [&](asio::error_code, std::size_t) { called = true; };
  using handler_type = decltype(handler);
  using op_type = rdma::detail::nd_connect_op<handler_type,
                                               asio::system_executor>;
  typename op_type::ptr p = {asio::detail::addressof(handler),
                             op_type::ptr::allocate(handler), 0};
  auto* invalid_connector = reinterpret_cast<IND2Connector*>(
      static_cast<std::uintptr_t>(1));
  p.p = new (p.v) op_type{invalid_connector, &state, asio::mutable_buffer{},
                          handler, asio::system_executor{}};
  auto* op = p.p;
  p.v = p.p = nullptr;

  op->destroy();

  ASIO_CHECK(!called);
  ASIO_CHECK(state.load() == rdma::detail::connect_state::connecting);
}

// Compile the poll-mode overload without requiring a physical device in this
// unit test. Runtime poll-mode success is covered by the hardware echo suite.
void compile_poll_move_accept(rdma::nd_connector<rdma::tcp>& connector,
                              rdma::nd_completion_queue& cq)
{
  connector.async_accept(
      cq, [](asio::error_code, rdma::nd_queue_pair) {});
}

void use_device_null_device()
{
  asio::io_context io;
  asio::error_code ec;

  rdma::use_device(io, rdma::nd_device_ptr{}, rdma::nd_config_t{}, ec);

  ASIO_CHECK(ec == rdma::rdma_errc::invalid_device);
}

void memory_region_null_device_throws()
{
  unsigned char storage[8] = {};

  try
  {
    rdma::nd_memory_region mr(rdma::nd_device_ptr{}, storage, sizeof(storage));
    (void)mr;
    ASIO_ERROR("nd_memory_region accepted a null device");
  }
  catch (std::system_error const& e)
  {
    ASIO_CHECK(e.code() == rdma::rdma_errc::invalid_device);
  }
}

ASIO_TEST_SUITE
(
  "nd/service_state_guards",
  ASIO_TEST_CASE(open_and_bind_without_use_device)
  ASIO_TEST_CASE(throwing_overloads_without_use_device)
  ASIO_TEST_CASE(default_queue_pair_state)
  ASIO_TEST_CASE(invalid_windows_handles_are_not_closable)
  ASIO_TEST_CASE(create_overlapped_file_checked_has_strict_failure_contract)
  ASIO_TEST_CASE(invalid_assign_handle)
  ASIO_TEST_CASE(listener_unopened_guards)
  ASIO_TEST_CASE(connector_unopened_disconnect_guards)
  ASIO_TEST_CASE(listener_unopened_async_get_connection_guards)
  ASIO_TEST_CASE(async_connect_without_use_device_completes_device_not_registered)
  ASIO_TEST_CASE(async_accept_without_use_device_completes_device_not_registered)
  ASIO_TEST_CASE(move_accept_without_use_device_returns_empty_queue_pair)
  ASIO_TEST_CASE(started_accept_failure_makes_connector_terminal)
  ASIO_TEST_CASE(connect_op_shutdown_does_not_dereference_connector)
  ASIO_TEST_CASE(use_device_null_device)
  ASIO_TEST_CASE(memory_region_null_device_throws)
)
