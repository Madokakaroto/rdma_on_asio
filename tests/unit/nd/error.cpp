#include <string>
#include <type_traits>

#include "unit_test.hpp"

#include "asio/error.hpp"
#include "rdma/nd/nd_error.hpp"

namespace rdma = asio::rdma;

void category_identity()
{
  auto ec = make_error_code(rdma::nd_errc::invalid_device_request);

  ASIO_CHECK(ec.category() == rdma::get_nd_error_category());
  ASIO_CHECK(std::string(ec.category().name()) == "nd_error_code");
}

void native_messages()
{
  auto success = make_error_code(rdma::nd_errc::success);
  auto cancelled = make_error_code(rdma::nd_errc::canceled);
  auto invalid = make_error_code(rdma::nd_errc::invalid_parameter);

  ASIO_CHECK(success.message() == "ND_SUCCESS");
  ASIO_CHECK(cancelled.message() == "ND_CANCELED");
  ASIO_CHECK(invalid.message() == "ND_INVALID_PARAMETER");
}

void pending_is_internal_diagnostic_only()
{
  auto pending = rdma::make_nd_error_code(ND_PENDING);

  ASIO_CHECK(pending.category() == rdma::get_nd_error_category());
  ASIO_CHECK(pending.value() == static_cast<int>(ND_PENDING));
  ASIO_CHECK(pending.message() == "UNKNOWN_ND_ERROR");
}

void native_cancel_is_not_user_visible_cancellation()
{
  asio::error_code native_cancel = rdma::nd_errc::canceled;
  asio::error_code user_cancel = asio::error::operation_aborted;

  ASIO_CHECK(native_cancel != user_cancel);
  ASIO_CHECK(user_cancel == asio::error::operation_aborted);
}

void enum_conversion()
{
  static_assert(std::is_error_code_enum_v<rdma::nd_errc>);

  asio::error_code ec = rdma::nd_errc::device_removed;
  ASIO_CHECK(ec == rdma::nd_errc::device_removed);
  ASIO_CHECK(ec != rdma::nd_errc::canceled);
}

ASIO_TEST_SUITE
(
  "nd/error",
  ASIO_TEST_CASE(category_identity)
  ASIO_TEST_CASE(native_messages)
  ASIO_TEST_CASE(pending_is_internal_diagnostic_only)
  ASIO_TEST_CASE(native_cancel_is_not_user_visible_cancellation)
  ASIO_TEST_CASE(enum_conversion)
)
