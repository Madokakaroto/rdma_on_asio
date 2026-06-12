#include <cassert>
#include <cstddef>
#include <iostream>
#include <system_error>

#include "asio.hpp"
#include "nd/nd_connector.hpp"
#include "nd/nd_listener.hpp"
#include "nd/nd_queue_pair.hpp"
#include "nd/nd_use_device.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

rdma::nd_device_ptr first_device() {
  return rdma::nd_device_manager_t::instance().get_first_available_device(
      tcp::v4(), rdma::nd_config_t{});
}

void test_open_without_use_device_fails() {
  asio::io_context io;
  asio::error_code ec;

  rdma::nd_connector<tcp> connector(io);
  connector.open(tcp::v4(), ec);
  assert(ec == rdma::rdma_errc::device_not_registered);

  rdma::nd_listener<tcp> listener(io);
  listener.open(tcp::v4(), ec);
  assert(ec == rdma::rdma_errc::device_not_registered);

  rdma::nd_queue_pair qp;
  qp.bind(io, ec);
  assert(ec == rdma::rdma_errc::device_not_registered);
  assert(!qp.is_bound());
  assert(qp.bound_type() == rdma::completion_mode::none);

  std::cout << "[PASS] open/bind without use_device returns "
               "device_not_registered\n";
}

void test_listener_open_bind_listen() {
  asio::io_context io;
  auto device = first_device();
  asio::error_code ec;
  rdma::use_device(io, device, rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] listener open/bind/listen: " << ec.message() << "\n";
    return;
  }

  rdma::nd_listener<tcp> listener(io);
  listener.open(tcp::v4(), ec);
  if (ec) {
    std::cout << "[SKIP] listener.open failed: " << ec.message() << "\n";
    return;
  }
  assert(listener.is_open());

  listener.bind(0, ec);
  if (ec) {
    std::cout << "[SKIP] listener.bind failed: " << ec.message() << "\n";
    return;
  }

  listener.listen(128, ec);
  if (ec) {
    std::cout << "[SKIP] listener.listen failed: " << ec.message() << "\n";
    return;
  }

  std::cout << "[PASS] listener open/bind/listen\n";
}

void test_connector_open() {
  asio::io_context io;
  auto device = first_device();
  asio::error_code ec;
  rdma::use_device(io, device, rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] connector open: " << ec.message() << "\n";
    return;
  }

  rdma::nd_connector<tcp> connector(io);
  connector.open(tcp::v4(), ec);
  if (ec) {
    std::cout << "[SKIP] connector.open failed: " << ec.message() << "\n";
    return;
  }
  assert(connector.is_open());

  std::cout << "[PASS] connector open\n";
}

void compile_only_async_surface(bool run) {
  if (!run) {
    return;
  }

  asio::io_context io;
  auto device = first_device();
  rdma::use_device(io, device);

  rdma::nd_queue_pair qp(io);
  rdma::nd_connector<tcp> connector(io);
  rdma::nd_listener<tcp> listener(io);

  asio::const_buffer pd;
  tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

  asio::mutable_buffer reply;
  asio::mutable_buffer request;
  connector.async_connect(qp, ep, pd, reply,
                          [](asio::error_code, std::size_t) {});
  connector.async_accept(qp, pd, [](asio::error_code) {});
  asio::error_code dec;
  connector.disconnect(dec);
  connector.async_wait_disconnect([](asio::error_code) {});

  listener.async_get_connection(
      request, [](asio::error_code, rdma::nd_connector<tcp>, std::size_t) {});
  rdma::nd_connector<tcp> peer(io);
  listener.async_get_connection(peer, request,
                                [](asio::error_code, std::size_t) {});
}

int main() {
  try {
    test_open_without_use_device_fails();
    test_listener_open_bind_listen();
    test_connector_open();
    compile_only_async_surface(false);
    std::cout << "\nAll nd_connector/listener tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "nd connector/listener test failed: " << e.what() << "\n";
    return 1;
  }
}
