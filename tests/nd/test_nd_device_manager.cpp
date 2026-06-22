#include <cassert>
#include <iostream>
#include <system_error>

#include "rdma/nd/nd_device.hpp"
#include "rdma/tcp.hpp"

void test_singleton() {
  auto const& mgr1 = asio::rdma::nd_device_manager_t::instance();
  auto const& mgr2 = asio::rdma::nd_device_manager_t::instance();
  assert(&mgr1 == &mgr2);
  std::cout << "[PASS] singleton: instance() returns same address\n";
}

// No port-space arg: one device per physical NIC (grouped by AdapterId),
// carrying its v4 and/or v6 local addresses -- v4/v6 are NOT separate devices.
// See docs/nd_dual_family_plan.md.
void test_get_first_available_device() {
  auto const& mgr = asio::rdma::nd_device_manager_t::instance();
  auto device = mgr.get_first_available_device(asio::rdma::nd_config_t{});
  if (device) {
    assert(device->adapter_ != nullptr);
    assert(!device->name_.empty());
    // A device must carry at least one local address (v4 and/or v6).
    assert(device->v4_addr_.has_value() || device->v6_addr_.has_value());
    std::cout << "[PASS] get_first_available_device: found device \""
              << device->name_ << "\" (v4="
              << (device->v4_addr_.has_value() ? "yes" : "no")
              << " v6=" << (device->v6_addr_.has_value() ? "yes" : "no")
              << ")\n";
  }
  else {
    std::cout << "[SKIP] get_first_available_device: no RDMA device available\n";
  }
}

void test_get_device_with_strict_config() {
  auto const& mgr = asio::rdma::nd_device_manager_t::instance();
  asio::rdma::nd_config_t config{};
  config.cqe_ = 0xFFFFFFFF;
  config.max_send_wr_ = 0xFFFFFFFF;
  auto device = mgr.get_first_available_device(config);
  assert(device == nullptr);
  std::cout << "[PASS] get_first_available_device: impossible config returns "
               "nullptr\n";
}

int main() {
  try {
    test_singleton();
    test_get_first_available_device();
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
