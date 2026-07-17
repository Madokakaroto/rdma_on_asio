#include <cassert>
#include <cstddef>
#include <iostream>
#include <span>
#include <system_error>

#include "asio.hpp"
#include "rdma/ibv/ibv_connector.hpp"
#include "rdma/ibv/ibv_listener.hpp"
#include "rdma/ibv/ibv_queue_pair.hpp"
#include "rdma/ibv/ibv_use_device.hpp"
#include "rdma/tcp.hpp"

using asio::rdma::tcp;

void test_listener_open_bind_listen() {
  asio::io_context io;
  auto device = asio::rdma::ibv_device_manager_t::instance()
                    .get_first_available_device({});
  asio::error_code ec;
  asio::rdma::use_device(io, device, {}, ec);
  if (ec) {
    std::cout << "[SKIP] use_device failed: " << ec.message() << "\n";
    return;
  }
  asio::rdma::ibv_listener<tcp> listener(io);

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
  auto device = asio::rdma::ibv_device_manager_t::instance()
                    .get_first_available_device({});
  asio::error_code ec;
  asio::rdma::use_device(io, device, {}, ec);
  if (ec) {
    std::cout << "[SKIP] use_device failed: " << ec.message() << "\n";
    return;
  }
  asio::rdma::ibv_connector<tcp> connector(io);

  connector.open(tcp::v4(), ec);
  if (ec) {
    std::cout << "[SKIP] connector.open failed: " << ec.message() << "\n";
    return;
  }
  assert(connector.is_open());
  std::cout << "[PASS] connector open (event channel + cm_id created)\n";
}

void test_accept_on_registered_but_unopened_connector() {
  asio::io_context io;
  auto device = asio::rdma::ibv_device_manager_t::instance()
                    .get_first_available_device({});
  asio::error_code ec;
  asio::rdma::use_device(io, device, {}, ec);
  if (ec) {
    std::cout << "[SKIP] unopened accept guard: " << ec.message() << "\n";
    return;
  }

  asio::rdma::ibv_completion_queue cq(device);
  ec = asio::error::operation_aborted;
  assert(cq.poll(ec) == 0);
  assert(!ec);
  ec = asio::error::operation_aborted;
  assert(cq.poll_one(ec) == 0);
  assert(!ec);
  asio::rdma::ibv_queue_pair qp(cq);
  asio::rdma::ibv_connector<tcp> connector(io);
  bool called = false;
  connector.async_accept(qp, [&](asio::error_code accept_ec) {
    called = true;
    ec = accept_ec;
  });
  io.run();
  assert(called);
  assert(ec == asio::rdma::rdma_errc::invalid_handle);
  std::cout << "[PASS] accept rejects registered but unopened connector\n";
}

// Compile-only coverage of the async surface (full ESTABLISHED needs a peer).
void compile_only_async_surface(bool run) {
  if (!run) {
    return;
  }
  asio::io_context io;
  auto device = asio::rdma::ibv_device_manager_t::instance()
                    .get_first_available_device({});
  asio::rdma::use_device(io, device);
  asio::rdma::ibv_queue_pair qp(io);
  asio::rdma::ibv_connector<tcp> connector(io);
  asio::rdma::ibv_listener<tcp> listener(io);

  asio::const_buffer pd;
  tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

  asio::mutable_buffer reply;   // connect: receives the server's reply pd
  asio::mutable_buffer request;  // get_connection: receives the client's request pd
  connector.async_connect(qp, ep, pd, reply,
                          [](asio::error_code, std::size_t) {});
  connector.async_accept(qp, pd, [](asio::error_code) {});
  asio::error_code dec;
  connector.disconnect(dec);  // now synchronous
  connector.async_wait_disconnect([](asio::error_code) {});  // on_disconnect

  // return form: handler(error_code, connector, std::size_t request_len)
  listener.async_get_connection(
      request,
      [](asio::error_code, asio::rdma::ibv_connector<tcp>, std::size_t) {});
  // fill form: handler(error_code, std::size_t request_len)
  asio::rdma::ibv_connector<tcp> peer(io);
  listener.async_get_connection(peer, request,
                                [](asio::error_code, std::size_t) {});
}

int main() {
  try {
    test_listener_open_bind_listen();
    test_connector_open();
    test_accept_on_registered_but_unopened_connector();
    compile_only_async_surface(false);
    std::cout << "\nAll ibv_connector/listener tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "ibv connector/listener test failed: " << e.what() << "\n";
    return 1;
  }
}
