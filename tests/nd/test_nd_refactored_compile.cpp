#include <iostream>

#include "asio/io_context.hpp"
#include "nd/nd_use_device.hpp"
#include "nd/nd_completion_queue.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

void test_type_sizes() {
  std::cout << "sizeof(nd_config_t) = " << sizeof(rdma::nd_config_t) << "\n";
  std::cout << "sizeof(nd_connector_handle_t) = "
            << sizeof(rdma::detail::nd_connector_handle_t) << "\n";
}

void test_io_objects_construct() {
  asio::io_context io_ctx;

  // These will fail at runtime without RDMA hardware, but must compile
  asio::error_code ec;
  auto device = rdma::nd_device_manager_t::instance()
                    .get_first_available_device(tcp::v4(), rdma::nd_config_t{});
  rdma::use_device(io_ctx, device, rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] no RDMA device, skipping IO object tests\n";
    return;
  }

  // nd_queue_pair (IOCP mode)
  rdma::nd_queue_pair qp(io_ctx);

  // nd_connector
  rdma::nd_connector<tcp> conn(io_ctx);
  conn.open(tcp::v4(), ec);
  if (ec) {
    std::cout << "[SKIP] connector.open failed: " << ec.message() << "\n";
    return;
  }

  // nd_listener
  rdma::nd_listener<tcp> listener(io_ctx);
  listener.open(tcp::v4(), ec);
  if (ec) {
    std::cout << "[SKIP] listener.open failed: " << ec.message() << "\n";
    return;
  }
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

  std::cout << "[PASS] all IO objects constructed\n";
}

void test_queue_pair_deferred() {
  asio::io_context io_ctx;
  asio::error_code ec;
  auto device = rdma::nd_device_manager_t::instance()
                    .get_first_available_device(tcp::v4(), rdma::nd_config_t{});
  rdma::use_device(io_ctx, device, rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] deferred test: no device\n";
    return;
  }

  rdma::nd_queue_pair qp;
  qp.bind(io_ctx);
  assert(qp.is_bound());
  std::cout << "[PASS] deferred queue_pair bind\n";
}

int main() {
  try {
    test_type_sizes();
    test_io_objects_construct();
    test_queue_pair_deferred();
    std::cout << "\nAll refactored compile tests passed.\n";
    return 0;
  } catch (std::system_error const& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
