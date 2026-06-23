#pragma once

#include "unit_test.hpp"

#include "asio/ip/tcp.hpp"
#include "rdma/nd/nd_types.hpp"

namespace {

decltype(&NdStartup) volatile nd_startup_symbol = &NdStartup;
decltype(&NdCleanup) volatile nd_cleanup_symbol = &NdCleanup;

}  // namespace

void ndutil_symbols_are_linked()
{
  ASIO_CHECK(nd_startup_symbol != nullptr);
  ASIO_CHECK(nd_cleanup_symbol != nullptr);
}

ASIO_TEST_SUITE
(
  "nd/asio_macro_link",
  ASIO_TEST_CASE(ndutil_symbols_are_linked)
)
