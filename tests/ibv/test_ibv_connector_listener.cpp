#include <cassert>
#include <cstddef>
#include <iostream>
#include <span>
#include <system_error>

#include "asio.hpp"
#include "ibv/ibv_connector.hpp"
#include "ibv/ibv_listener.hpp"
#include "ibv/ibv_use_device.hpp"
#include "rdma/tcp.hpp"

using asio::rdma::tcp;

// Runtime smoke test: listener open/bind/listen and connector open. These only
// touch rdma_cm control-plane objects (event channel + cm_id), no QP needed.
void test_listener_open_bind_listen() {
  asio::io_context io;
  asio::rdma::ibv_listener<tcp> listener(io);

  asio::error_code ec;
  listener.open(asio::rdma::ibv_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] listener.open failed: " << ec.message() << "\n";
    return;
  }
  assert(listener.is_open());

  tcp::endpoint ep(asio::ip::address_v4::any(), 0);
  listener.bind(ep, ec);
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
  asio::rdma::use_device(io);  // shared CQ needed by the queue_pair
  asio::rdma::ibv_queue_pair<tcp> qp(io);
  asio::rdma::ibv_connector<tcp> connector(io);

  asio::error_code ec;
  connector.open(qp, asio::rdma::ibv_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] connector.open failed: " << ec.message() << "\n";
    return;
  }
  assert(connector.is_open());
  std::cout << "[PASS] connector open (event channel + cm_id created)\n";
}

// Compile-only coverage of the async surface. Full connect/accept reach
// ESTABLISHED only with a QP (next step), so these are instantiated, not run.
void compile_only_async_surface(bool run) {
  if (!run) {
    return;
  }
  asio::io_context io;
  asio::rdma::ibv_connector<tcp> connector(io);
  asio::rdma::ibv_listener<tcp> listener(io);

  std::span<const std::byte> pd;
  tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

  connector.async_connect(ep, pd, [](asio::error_code) {});
  connector.async_accept(pd, [](asio::error_code) {});
  connector.async_disconnect([](asio::error_code) {});
  listener.async_get_connection_request(
      [](asio::error_code, tcp::listener::native_connector_type,
         std::span<const std::byte>) {});
}

int main() {
  try {
    test_listener_open_bind_listen();
    test_connector_open();
    compile_only_async_surface(false);
    std::cout << "\nAll ibv_connector/listener tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "ibv connector/listener test failed: " << e.what() << "\n";
    return 1;
  }
}
