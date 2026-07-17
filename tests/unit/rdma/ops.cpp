#include <array>
#include <cstddef>
#include <new>
#include <memory>
#include <vector>

#include "unit_test.hpp"

#include "asio/error.hpp"
#include "asio/system_executor.hpp"
#include "rdma/detail/rdma_op_read.hpp"
#include "rdma/detail/rdma_op_recv.hpp"
#include "rdma/detail/rdma_op_send.hpp"
#include "rdma/detail/rdma_op_write.hpp"

namespace rdma = asio::rdma;

struct recording_handler
{
  bool* called = nullptr;
  asio::error_code* ec = nullptr;
  std::size_t* bytes = nullptr;

  void operator()(asio::error_code e, std::size_t n)
  {
    *called = true;
    *ec = e;
    *bytes = n;
  }
};

struct counting_const_sequence
{
  std::array<rdma::const_buffer, 2> buffers{};
  std::shared_ptr<int> traversals;

  auto cbegin() const
  {
    ++*traversals;
    return buffers.cbegin();
  }

  auto cend() const
  {
    ++*traversals;
    return buffers.cend();
  }
};

template <typename Op, typename Handler, typename... Args>
Op* allocate_op(Handler& handler, Args&&... args)
{
  typename Op::ptr p = {asio::detail::addressof(handler),
                        Op::ptr::allocate(handler), 0};
  p.p = new (p.v) Op(std::forward<Args>(args)...);
  Op* raw = p.p;
  p.v = 0;
  p.p = 0;
  return raw;
}

std::vector<rdma::const_buffer> const_buffers()
{
  static std::array<unsigned char, 16> storage{};
  return {
      rdma::const_buffer(storage.data(), 4, 10),
      rdma::const_buffer(storage.data() + 4, 6, 11),
  };
}

std::vector<rdma::mutable_buffer> mutable_buffers()
{
  static std::array<unsigned char, 16> storage{};
  return {
      rdma::mutable_buffer(storage.data(), 3, 20),
      rdma::mutable_buffer(storage.data() + 3, 5, 21),
  };
}

void send_op_reports_buffer_size_on_success()
{
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 0;
  recording_handler handler{&called, &ec, &bytes};
  auto buffers = const_buffers();
  using op_type =
      rdma::detail::rdma_send_op<decltype(buffers), recording_handler,
                                 asio::system_executor>;

  auto* op = allocate_op<op_type>(handler, asio::error_code{}, buffers,
                                  handler, asio::system_executor{});
  ASIO_CHECK(op->get_op_type() ==
             rdma::detail::rdma_verbs_op_base::op_type::post_send);
  ASIO_CHECK(rdma::detail::buffer_size(op->get_buffer_sequence()) == 10);

  op->set_posted_bytes(10);
  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(!ec);
  ASIO_CHECK(bytes == 10);
}

void send_op_preserves_error_and_zero_bytes()
{
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 99;
  recording_handler handler{&called, &ec, &bytes};
  auto buffers = const_buffers();
  auto aborted = asio::error_code(asio::error::operation_aborted);
  using op_type =
      rdma::detail::rdma_send_op<decltype(buffers), recording_handler,
                                 asio::system_executor>;

  auto* op = allocate_op<op_type>(handler, aborted, buffers, handler,
                                  asio::system_executor{});
  op->bytes_transferred_ = 0;

  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(ec == asio::error::operation_aborted);
  ASIO_CHECK(bytes == 0);
}

void recv_op_reports_completion_bytes()
{
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 0;
  recording_handler handler{&called, &ec, &bytes};
  auto buffers = mutable_buffers();
  using op_type =
      rdma::detail::rdma_recv_op<decltype(buffers), recording_handler,
                                 asio::system_executor>;

  auto* op = allocate_op<op_type>(handler, asio::error_code{}, buffers,
                                  handler, asio::system_executor{});
  ASIO_CHECK(op->get_op_type() ==
             rdma::detail::rdma_verbs_op_base::op_type::post_recv);
  op->bytes_transferred_ = 5;

  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(!ec);
  ASIO_CHECK(bytes == 5);
}

void write_op_reports_buffer_size_and_remote_addr()
{
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 0;
  recording_handler handler{&called, &ec, &bytes};
  auto buffers = const_buffers();
  rdma::rdma_remote_addr_t remote{0x12340000, 0x77};
  using op_type =
      rdma::detail::rdma_write_op<decltype(buffers), recording_handler,
                                  asio::system_executor>;

  auto* op = allocate_op<op_type>(handler, asio::error_code{}, buffers,
                                  remote, handler, asio::system_executor{});
  ASIO_CHECK(op->get_op_type() ==
             rdma::detail::rdma_verbs_op_base::op_type::remote_write);
  ASIO_CHECK(op->get_remote_addr().addr_ == remote.addr_);
  ASIO_CHECK(op->get_remote_addr().token_ == remote.token_);

  op->set_posted_bytes(10);
  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(!ec);
  ASIO_CHECK(bytes == 10);
}

void read_op_reports_completion_bytes_and_remote_addr()
{
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 0;
  recording_handler handler{&called, &ec, &bytes};
  auto buffers = mutable_buffers();
  rdma::rdma_remote_addr_t remote{0x98760000, 0x88};
  using op_type =
      rdma::detail::rdma_read_op<decltype(buffers), recording_handler,
                                 asio::system_executor>;

  auto* op = allocate_op<op_type>(handler, asio::error_code{}, buffers, remote,
                                  handler, asio::system_executor{});
  ASIO_CHECK(op->get_op_type() ==
             rdma::detail::rdma_verbs_op_base::op_type::remote_read);
  ASIO_CHECK(op->get_remote_addr().addr_ == remote.addr_);
  ASIO_CHECK(op->get_remote_addr().token_ == remote.token_);
  op->bytes_transferred_ = 7;

  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(!ec);
  ASIO_CHECK(bytes == 7);
}

void send_completion_does_not_revisit_buffer_sequence()
{
  static std::array<unsigned char, 10> storage{};
  auto traversals = std::make_shared<int>(0);
  counting_const_sequence buffers{
      {rdma::const_buffer(storage.data(), 4, 1),
       rdma::const_buffer(storage.data() + 4, 6, 2)},
      traversals};
  bool called = false;
  asio::error_code ec;
  std::size_t bytes = 0;
  recording_handler handler{&called, &ec, &bytes};
  using op_type = rdma::detail::rdma_send_op<
      counting_const_sequence, recording_handler, asio::system_executor>;
  auto* op = allocate_op<op_type>(handler, asio::error_code{}, buffers,
                                  handler, asio::system_executor{});

  auto const posted_bytes = rdma::detail::buffer_size(buffers);
  op->set_posted_bytes(posted_bytes);
  auto const before_completion = *traversals;
  int owner = 0;
  op->complete(&owner);

  ASIO_CHECK(called);
  ASIO_CHECK(!ec);
  ASIO_CHECK(bytes == 10);
  ASIO_CHECK(*traversals == before_completion);
}

ASIO_TEST_SUITE
(
  "rdma/ops",
  ASIO_TEST_CASE(send_op_reports_buffer_size_on_success)
  ASIO_TEST_CASE(send_op_preserves_error_and_zero_bytes)
  ASIO_TEST_CASE(recv_op_reports_completion_bytes)
  ASIO_TEST_CASE(write_op_reports_buffer_size_and_remote_addr)
  ASIO_TEST_CASE(read_op_reports_completion_bytes_and_remote_addr)
  ASIO_TEST_CASE(send_completion_does_not_revisit_buffer_sequence)
)
