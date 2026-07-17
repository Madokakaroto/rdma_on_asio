#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <system_error>

#include "rdma/rdma.hpp"

void test_singleton() {
  auto const& mgr1 = asio::rdma::ibv_device_manager_t::instance();
  auto const& mgr2 = asio::rdma::ibv_device_manager_t::instance();
  assert(&mgr1 == &mgr2);
  std::cout << "[PASS] singleton: instance() returns same address\n";
}

// No port-space arg: a verbs device is family-agnostic and serves both v4 and v6
// (family is consumed at rdma_cm connect time). See docs/nd_dual_family_plan.md.
void test_get_first_available_device() {
  auto const& mgr = asio::rdma::ibv_device_manager_t::instance();
  auto device = mgr.get_first_available_device(asio::rdma::ibv_config_t{});
  if (device) {
    auto address = device->get_v4_address();
    assert(!address.is_unspecified());
    std::cout << "[PASS] get_first_available_device: found device with local "
              << address.to_string() << "\n";
  }
  else {
    std::cout << "[SKIP] get_first_available_device: no RDMA device available\n";
  }
}

void test_get_device_with_strict_config() {
  auto const& mgr = asio::rdma::ibv_device_manager_t::instance();
  asio::rdma::ibv_config_t config{};
  config.cqe_ = 0xFFFFFFFF;
  config.max_send_wr_ = 0xFFFFFFFF;
  auto device = mgr.get_first_available_device(config);
  assert(device == nullptr);
  std::cout << "[PASS] get_first_available_device: impossible config returns "
               "nullptr\n";
}

void test_explicit_device_rejects_incompatible_config() {
  auto device = asio::rdma::ibv_device_manager_t::instance()
                    .get_first_available_device({});
  if (!device) {
    std::cout << "[SKIP] invalid explicit config: no device\n";
    return;
  }
  asio::io_context io;
  asio::rdma::ibv_config_t config{};
  config.cqe_ = (std::numeric_limits<std::uint32_t>::max)();
  asio::error_code ec;
  asio::rdma::use_device(io, device, config, ec);
  assert(ec == asio::rdma::rdma_errc::invalid_config);
  std::cout << "[PASS] explicit device rejects incompatible config\n";
}

int main() {
  try {
    test_singleton();
    test_get_first_available_device();
    test_get_device_with_strict_config();
    test_explicit_device_rejects_incompatible_config();
    std::cout << "\nAll ibv_device_manager tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "ibv_device_manager init failed: " << e.what()
              << " (no RDMA device available?)\n";
    return 1;
  }
}
