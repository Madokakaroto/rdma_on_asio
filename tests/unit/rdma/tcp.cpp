#include <type_traits>

#include "unit_test.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;

void v4_any_endpoint()
{
  auto ep = rdma::tcp::v4().any_endpoint(5042);
  ASIO_CHECK(ep.port() == 5042);
  ASIO_CHECK(ep.address().is_v4());
  ASIO_CHECK(ep.address().is_unspecified());
}

void v6_any_endpoint()
{
  auto ep = rdma::tcp::v6().any_endpoint(5043);
  ASIO_CHECK(ep.port() == 5043);
  ASIO_CHECK(ep.address().is_v6());
  ASIO_CHECK(ep.address().is_unspecified());
}

void backend_aliases_compile()
{
  using connector = typename rdma::tcp::connector;
  using listener = typename rdma::tcp::listener;

  static_assert(std::is_same_v<connector, rdma::rdma_connector<rdma::tcp>>);
  static_assert(std::is_same_v<listener, rdma::rdma_listener<rdma::tcp>>);
}

ASIO_TEST_SUITE
(
  "rdma/tcp",
  ASIO_TEST_CASE(v4_any_endpoint)
  ASIO_TEST_CASE(v6_any_endpoint)
  ASIO_COMPILE_TEST_CASE(backend_aliases_compile)
)
