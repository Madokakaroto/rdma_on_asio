#include <cassert>
#include <iostream>
#include <string>

#include "nd/nd_error.hpp"

namespace rdma = asio::rdma;

void test_category_identity() {
  auto ec = make_error_code(rdma::nd_errc::ext_invalid_device);

  assert(ec.category() == rdma::get_nd_error_category());
  assert(std::string(ec.category().name()) == "nd_error_code");

  std::cout << "[PASS] nd_errc maps to the ND error category\n";
}

void test_extension_messages() {
  auto disconnected = make_error_code(rdma::nd_errc::ext_disconnected);
  auto terminal = make_error_code(rdma::nd_errc::ext_connector_terminal);

  assert(disconnected.message() == "ND_EXT connection disconnected");
  assert(terminal.message() ==
         "ND_EXT connector is terminal (disconnected/failed); create a new connector");

  std::cout << "[PASS] extension error messages are stable\n";
}

void test_native_messages() {
  auto success = make_error_code(rdma::nd_errc::success);
  auto cancelled = make_error_code(rdma::nd_errc::canceled);

  assert(success.message() == "ND_SUCCESS");
  assert(cancelled.message() == "ND_CANCELED");

  std::cout << "[PASS] native ND status messages are stable\n";
}

int main() {
  test_category_identity();
  test_extension_messages();
  test_native_messages();

  std::cout << "\nAll nd_error tests passed.\n";
  return 0;
}
