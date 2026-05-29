#include <cassert>
#include <iostream>
#include <system_error>

#include "nd/nd_device.hpp"
#include "rdma/tcp.hpp"

void test_singleton() {
  auto const& mgr1 = asio::rdma::nd_device_manager_t::instance();
  auto const& mgr2 = asio::rdma::nd_device_manager_t::instance();
  assert(&mgr1 == &mgr2);
  std::cout << "[PASS] singleton: instance() returns same address\n";
}

void test_get_first_available_device_v4() {
  auto const& mgr = asio::rdma::nd_device_manager_t::instance();
  asio::rdma::nd_config_t config{};
  auto device =
      mgr.get_first_available_device(asio::rdma::tcp::v4(), config);
  if (device) {
    assert(device->adapter_ != nullptr);
    assert(!device->name_.empty());
    std::cout << "[PASS] get_first_available_device(v4): found device \""
              << device->name_ << "\"\n";
  }
  else {
    std::cout << "[SKIP] get_first_available_device(v4): no RDMA device "
                 "available\n";
  }
}

void test_get_first_available_device_v6() {
  auto const& mgr = asio::rdma::nd_device_manager_t::instance();
  asio::rdma::nd_config_t config{};
  auto device =
      mgr.get_first_available_device(asio::rdma::tcp::v6(), config);
  if (device) {
    assert(device->adapter_ != nullptr);
    assert(!device->name_.empty());
    std::cout << "[PASS] get_first_available_device(v6): found device \""
              << device->name_ << "\"\n";
  }
  else {
    std::cout << "[SKIP] get_first_available_device(v6): no RDMA device "
                 "available\n";
  }
}

void test_get_device_with_strict_config() {
  auto const& mgr = asio::rdma::nd_device_manager_t::instance();
  asio::rdma::nd_config_t config{};
  config.cqe_ = 0xFFFFFFFF;
  config.max_send_wr_ = 0xFFFFFFFF;
  auto device =
      mgr.get_first_available_device(asio::rdma::tcp::v4(), config);
  assert(device == nullptr);
  std::cout << "[PASS] get_first_available_device: impossible config returns "
               "nullptr\n";
}

int main() {
  try {
    test_singleton();
    test_get_first_available_device_v4();
    test_get_first_available_device_v6();
    test_get_device_with_strict_config();
    std::cout << "\nAll nd_device_manager tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "nd_device_manager init failed: " << e.what()
              << " (no RDMA provider installed?)\n";
    return 1;
  }
}
