#include <array>
#include <cassert>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/read.hpp"
#include "asio/write.hpp"
#include "rdma/nd/nd_device.hpp"

namespace rdma = asio::rdma;
using asio_tcp = asio::ip::tcp;

struct skip_test : std::runtime_error {
  using std::runtime_error::runtime_error;
};

rdma::nd_device_ptr create_nd_device_or_skip() {
  try {
    auto device = rdma::nd_device_manager_t::instance()
                      .get_first_available_device(rdma::nd_config_t{});
    if (!device) {
      throw skip_test("no NetworkDirect device available");
    }
    return device;
  } catch (std::system_error const& e) {
    throw skip_test(std::string("NetworkDirect device discovery failed: ") +
                    e.what());
  }
}

void run_socket_echo() {
  constexpr std::string_view payload =
      "plain asio tcp echo after nd_device creation";
  static_assert(payload.size() < 256);

  asio::io_context server_io;
  asio_tcp::acceptor acceptor(server_io);
  acceptor.open(asio_tcp::v4());
  acceptor.set_option(asio_tcp::acceptor::reuse_address(true));
  acceptor.bind(asio_tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  acceptor.listen(1);

  auto const endpoint = acceptor.local_endpoint();
  std::exception_ptr server_error;
  std::thread server([&] {
    try {
      asio_tcp::socket server_socket(server_io);
      acceptor.accept(server_socket);

      std::array<char, 256> buffer{};
      asio::read(server_socket,
                 asio::buffer(buffer.data(), payload.size()));
      asio::write(server_socket,
                  asio::buffer(buffer.data(), payload.size()));
    } catch (...) {
      server_error = std::current_exception();
    }
  });

  try {
    asio::io_context client_io;
    asio_tcp::socket client_socket(client_io);
    client_socket.connect(endpoint);

    asio::write(client_socket,
                asio::buffer(payload.data(), payload.size()));

    std::array<char, 256> reply{};
    asio::read(client_socket, asio::buffer(reply.data(), payload.size()));
    assert(std::string_view(reply.data(), payload.size()) == payload);

    asio::error_code ignored;
    client_socket.shutdown(asio_tcp::socket::shutdown_both, ignored);
    client_socket.close(ignored);
  } catch (...) {
    asio::error_code ignored;
    acceptor.close(ignored);
    if (server.joinable()) {
      server.join();
    }
    throw;
  }

  if (server.joinable()) {
    server.join();
  }
  if (server_error) {
    std::rethrow_exception(server_error);
  }
}

int main() {
  try {
    auto device = create_nd_device_or_skip();
    assert(device != nullptr);
    std::cout << "[PASS] nd_device created without use_device/io objects\n";

    run_socket_echo();
    std::cout << "[PASS] plain asio tcp socket echo works\n";
    std::cout << "\nAll nd socket echo without RDMA IO tests passed.\n";
    return 0;
  } catch (skip_test const& e) {
    std::cout << "[SKIP] " << e.what() << "\n";
    return 77;
  } catch (std::exception const& e) {
    std::cerr << "[FAIL] " << e.what() << "\n";
    return 1;
  }
}
