#include <string>
#include <type_traits>

#include "unit_test.hpp"

#include "asio/error_code.hpp"
#include "rdma/rdma_error.hpp"

namespace rdma = asio::rdma;

void category_identity()
{
  auto ec = make_error_code(rdma::rdma_errc::too_many_sge);

  ASIO_CHECK(ec.category() == rdma::get_rdma_error_category());
  ASIO_CHECK(std::string(ec.category().name()) == "rdma_error_code");
  ASIO_CHECK(ec);
}

void messages_are_backend_neutral()
{
  auto invalid = make_error_code(rdma::rdma_errc::invalid_handle);
  auto terminal = make_error_code(rdma::rdma_errc::connector_terminal);

  ASIO_CHECK(invalid.message() == "RDMA invalid object handle");
  ASIO_CHECK(terminal.message() ==
             "RDMA connector is terminal; create a new connector");
  ASIO_CHECK(invalid.message().find("ND") == std::string::npos);
  ASIO_CHECK(invalid.message().find("IBV") == std::string::npos);
}

void conversion_and_comparison()
{
  static_assert(std::is_error_code_enum_v<rdma::rdma_errc>);

  asio::error_code ec = rdma::rdma_errc::too_many_sge;
  ASIO_CHECK(ec == rdma::rdma_errc::too_many_sge);
  ASIO_CHECK(ec != rdma::rdma_errc::connector_terminal);

  ec = make_error_code(rdma::rdma_errc::invalid_handle);
  ASIO_CHECK(ec == rdma::rdma_errc::invalid_handle);

  asio::error_code success;
  ASIO_CHECK(!success);
}

void no_success_enumerator()
{
  ASIO_CHECK(static_cast<int>(rdma::rdma_errc::no_available_device) != 0);
}

ASIO_TEST_SUITE
(
  "rdma/error",
  ASIO_TEST_CASE(category_identity)
  ASIO_TEST_CASE(messages_are_backend_neutral)
  ASIO_TEST_CASE(conversion_and_comparison)
  ASIO_TEST_CASE(no_success_enumerator)
)
