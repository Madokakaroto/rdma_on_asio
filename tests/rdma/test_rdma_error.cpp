#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;

void test_category_identity() {
  auto ec = make_error_code(rdma::rdma_errc::too_many_sge);

  assert(ec.category() == rdma::get_rdma_error_category());
  assert(std::string(ec.category().name()) == "rdma_error_code");
  assert(ec);

  std::cout << "[PASS] rdma_errc maps to the shared RDMA error category\n";
}

void test_messages() {
  auto invalid = make_error_code(rdma::rdma_errc::invalid_handle);
  auto terminal = make_error_code(rdma::rdma_errc::connector_terminal);

  assert(invalid.message() == "RDMA invalid object handle");
  assert(terminal.message() ==
         "RDMA connector is terminal; create a new connector");
  assert(invalid.message().find("ND_EXT") == std::string::npos);
  assert(invalid.message().find("IBV_EXT") == std::string::npos);

  std::cout << "[PASS] shared RDMA messages are backend-neutral\n";
}

void test_implicit_conversion_and_comparison() {
  static_assert(std::is_error_code_enum_v<rdma::rdma_errc>);

  asio::error_code ec = rdma::rdma_errc::too_many_sge;
  assert(ec == rdma::rdma_errc::too_many_sge);
  assert(ec != rdma::rdma_errc::connector_terminal);

  ec = make_error_code(rdma::rdma_errc::invalid_handle);
  assert(ec == rdma::rdma_errc::invalid_handle);

  asio::error_code success{};
  assert(!success);

  std::cout << "[PASS] implicit conversion, ADL, and comparisons work\n";
}

int main() {
  test_category_identity();
  test_messages();
  test_implicit_conversion_and_comparison();

  std::cout << "\nAll rdma_error tests passed.\n";
  return 0;
}
