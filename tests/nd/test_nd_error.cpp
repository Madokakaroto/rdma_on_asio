#include <cassert>
#include <iostream>
#include <string>

#include "rdma/nd/nd_error.hpp"

namespace rdma = asio::rdma;

void test_category_identity() {
  auto ec = make_error_code(rdma::nd_errc::invalid_device_request);

  assert(ec.category() == rdma::get_nd_error_category());
  assert(std::string(ec.category().name()) == "nd_error_code");

  std::cout << "[PASS] nd_errc maps to the ND error category\n";
}

void test_native_messages() {
  auto success = make_error_code(rdma::nd_errc::success);
  auto cancelled = make_error_code(rdma::nd_errc::canceled);
  auto pending = rdma::make_nd_error_code(ND_PENDING);

  assert(success.message() == "ND_SUCCESS");
  assert(cancelled.message() == "ND_CANCELED");
  assert(pending.message() == "UNKNOWN_ND_ERROR");

  std::cout << "[PASS] native ND status messages are stable; ND_PENDING is internal\n";
}

int main() {
  test_category_identity();
  test_native_messages();

  std::cout << "\nAll nd_error tests passed.\n";
  return 0;
}
