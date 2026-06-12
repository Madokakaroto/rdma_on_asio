#include <cerrno>
#include <string>

#include "unit_test.hpp"

#include "rdma/ibv/ibv_error.hpp"

namespace rdma = asio::rdma;

void make_system_error_code_uses_system_category()
{
  auto ec = rdma::make_system_error_code(EINVAL);

  ASIO_CHECK(ec.category() == std::system_category());
  ASIO_CHECK(ec.value() == EINVAL);
  ASIO_CHECK(!ec.message().empty());
}

void last_system_error_captures_errno()
{
  errno = EAGAIN;

  auto ec = rdma::last_system_error();

  ASIO_CHECK(ec.category() == std::system_category());
  ASIO_CHECK(ec.value() == EAGAIN);
  ASIO_CHECK(!ec.message().empty());
}

ASIO_TEST_SUITE
(
  "ibv/error_helpers",
  ASIO_TEST_CASE(make_system_error_code_uses_system_category)
  ASIO_TEST_CASE(last_system_error_captures_errno)
)
