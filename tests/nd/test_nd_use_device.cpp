#include <cassert>
#include <iostream>
#include <optional>
#include <system_error>

#include "asio/io_context.hpp"
#include "nd/nd_use_device.hpp"

void test_use_device_default_config() {
  asio::io_context io_ctx;
  asio::error_code ec;
  auto& svc = asio::rdma::use_device(io_ctx, asio::rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] use_device(default config): " << ec.message()
              << " (no RDMA device?)\n";
    return;
  }
  assert(svc.is_initialized());
  assert(svc.get_adapter() != nullptr);
  assert(svc.get_cq() != nullptr);
  std::cout << "[PASS] use_device(default config)\n";
}

void test_use_device_double_init() {
  asio::io_context io_ctx;
  asio::error_code ec;
  asio::rdma::use_device(io_ctx, asio::rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] use_device double init: first call failed\n";
    return;
  }
  asio::rdma::use_device(io_ctx, asio::rdma::nd_config_t{}, ec);
  assert(ec);
  std::cout << "[PASS] use_device: double init returns error\n";
}

void test_use_device_with_selector() {
  asio::io_context io_ctx;
  asio::error_code ec;
  bool selector_called = false;

  asio::rdma::use_device(io_ctx,
      [&](asio::rdma::nd_device_ptr const& dev) -> std::optional<asio::rdma::nd_config_t> {
        selector_called = true;
        asio::rdma::nd_config_t config;
        config.cqe_ = 64;
        return config;
      }, ec);

  if (ec && !selector_called) {
    std::cout << "[SKIP] use_device(selector): no devices to iterate\n";
    return;
  }
  if (ec) {
    std::cout << "[SKIP] use_device(selector): " << ec.message() << "\n";
    return;
  }
  assert(selector_called);
  std::cout << "[PASS] use_device(selector)\n";
}

void test_effective_config() {
  asio::io_context io_ctx;
  asio::error_code ec;
  auto& svc = asio::rdma::use_device(io_ctx, asio::rdma::nd_config_t{}, ec);
  if (ec) {
    std::cout << "[SKIP] effective_config: no device\n";
    return;
  }
  auto const& cfg = svc.get_effective_config();
  assert(cfg.cqe_ > 0);
  assert(cfg.max_send_wr_ > 0);
  assert(cfg.max_recv_wr_ > 0);
  assert(cfg.max_send_sge_ > 0);
  assert(cfg.max_recv_sge_ > 0);
  std::cout << "[PASS] effective_config: all fields derived (cqe="
            << cfg.cqe_ << ")\n";
}

int main() {
  try {
    test_use_device_default_config();
    test_use_device_double_init();
    test_use_device_with_selector();
    test_effective_config();
    std::cout << "\nAll nd_use_device tests passed.\n";
    return 0;
  } catch (std::system_error const& e) {
    std::cerr << "test failed: " << e.what() << "\n";
    return 1;
  }
}
