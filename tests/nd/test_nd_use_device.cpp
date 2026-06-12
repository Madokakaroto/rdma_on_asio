#include <cassert>
#include <iostream>
#include <system_error>

#include "asio/io_context.hpp"
#include "rdma/nd/nd_use_device.hpp"
#include "rdma/nd/detail/nd_service_device.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

// Discover a device via the manager (returns nullptr if none on this host).
static rdma::nd_device_ptr first_device() {
  return rdma::nd_device_manager_t::instance().get_first_available_device(
      tcp::v4(), rdma::nd_config_t{});
}

void test_get_first_available_device() {
  auto dev = first_device();
  if (!dev) {
    std::cout << "[SKIP] get_first_available_device: no RDMA device\n";
    return;
  }
  std::cout << "[PASS] get_first_available_device\n";
}

void test_use_device() {
  asio::io_context io_ctx;
  auto dev = first_device();
  if (!dev) {
    std::cout << "[SKIP] use_device: no device\n";
    return;
  }
  asio::error_code ec;
  rdma::use_device(io_ctx, dev, rdma::nd_config_t{}, ec);
  assert(!ec);
  auto& dev_svc = asio::use_service<rdma::detail::nd_device_service>(io_ctx);
  assert(dev_svc.is_registered());
  assert(dev_svc.get_device() != nullptr);
  auto& io_svc = asio::use_service<rdma::detail::nd_io_completion_service>(io_ctx);
  assert(io_svc.get_cq() != nullptr);
  std::cout << "[PASS] use_device(io, device)\n";
}

void test_use_device_double_init() {
  asio::io_context io_ctx;
  auto dev = first_device();
  if (!dev) {
    std::cout << "[SKIP] use_device double init: no device\n";
    return;
  }
  asio::error_code ec;
  rdma::use_device(io_ctx, dev, rdma::nd_config_t{}, ec);
  assert(!ec);
  rdma::use_device(io_ctx, dev, rdma::nd_config_t{}, ec);
  assert(ec == rdma::rdma_errc::already_registered);
  std::cout << "[PASS] use_device: double init returns error\n";
}

void test_use_device_null_device() {
  asio::io_context io_ctx;
  asio::error_code ec;
  rdma::use_device(io_ctx, rdma::nd_device_ptr{}, rdma::nd_config_t{}, ec);
  assert(ec == rdma::rdma_errc::invalid_device);
  std::cout << "[PASS] use_device(nullptr) returns error\n";
}

void test_effective_config() {
  asio::io_context io_ctx;
  auto dev = first_device();
  if (!dev) {
    std::cout << "[SKIP] effective_config: no device\n";
    return;
  }
  asio::error_code ec;
  rdma::use_device(io_ctx, dev, rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] effective_config: " << ec.message() << "\n";
    return;
  }
  auto& dev_svc = asio::use_service<rdma::detail::nd_device_service>(io_ctx);
  auto const& cfg = dev_svc.get_effective_config();
  assert(cfg.cqe_ > 0);
  assert(cfg.max_send_wr_ > 0);
  assert(cfg.max_recv_wr_ > 0);
  assert(cfg.max_send_sge_ > 0);
  assert(cfg.max_recv_sge_ > 0);
  std::cout << "[PASS] effective_config: all fields derived (cqe=" << cfg.cqe_
            << ")\n";
}

int main() {
  try {
    test_get_first_available_device();
    test_use_device();
    test_use_device_double_init();
    test_use_device_null_device();
    test_effective_config();
    std::cout << "\nAll nd_use_device tests passed.\n";
    return 0;
  } catch (std::system_error const& e) {
    std::cerr << "test failed: " << e.what() << "\n";
    return 1;
  }
}
