#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> server_loop(rdma_bench::options opt,
                                  std::atomic<std::uint64_t>& completed,
                                  std::atomic<std::uint64_t>& errors) {
  auto ex = co_await asio::this_coro::executor;
  auto& io = static_cast<asio::io_context&>(ex.context());
  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(opt.port);
  listener.listen();
  std::cout << "RDMA_BENCH_READY role=server stress=connect_disconnect port="
            << opt.port << "\n";

  for (std::uint64_t i = 0; i < opt.iterations; ++i) {
    auto [ecg, conn, req_len] =
        co_await listener.async_get_connection(asio::mutable_buffer{},
                                               nothrow);
    (void)req_len;
    if (ecg) {
      ++errors;
      co_return;
    }
    rdma::rdma_queue_pair qp(io);
    auto [eca] = co_await conn.async_accept(qp, asio::const_buffer{}, nothrow);
    if (eca) {
      ++errors;
      co_return;
    }
    co_await conn.async_wait_disconnect(nothrow);
    ++completed;
  }
}

asio::awaitable<void> client_loop(rdma_bench::options opt,
                                  std::atomic<std::uint64_t>& completed,
                                  std::atomic<std::uint64_t>& errors) {
  auto ex = co_await asio::this_coro::executor;
  auto& io = static_cast<asio::io_context&>(ex.context());
  for (std::uint64_t i = 0; i < opt.iterations; ++i) {
    rdma::rdma_connector<tcp> conn(io);
    conn.open(tcp::v4());
    rdma::rdma_queue_pair qp(io);
    tcp::endpoint ep(asio::ip::make_address(opt.local_addr), opt.port);
    auto [ecc, reply_len] = co_await conn.async_connect(
        qp, ep, asio::const_buffer{}, asio::mutable_buffer{}, nothrow);
    (void)reply_len;
    if (ecc) {
      ++errors;
      co_return;
    }
    conn.disconnect();
    ++completed;
  }
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto cmd = rdma_bench::command_line(argc, argv);
    if (opt.local_addr.empty()) {
      throw std::invalid_argument("--local-addr is required");
    }

    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    std::atomic<std::uint64_t> server_completed{0};
    std::atomic<std::uint64_t> client_completed{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<int> remaining{2};
    auto on_done = [&](std::exception_ptr e) {
      if (e) ++errors;
      if (--remaining == 0) io.stop();
    };

    asio::co_spawn(io, server_loop(opt, server_completed, errors), on_done);
    asio::co_spawn(io, client_loop(opt, client_completed, errors), on_done);

    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(opt.timeout_sec));
    watchdog.async_wait([&](asio::error_code ec) {
      if (!ec) {
        ++errors;
        io.stop();
      }
    });

    std::vector<std::thread> threads;
    auto thread_count = std::max<std::uint32_t>(1, opt.threads);
    for (std::uint32_t i = 0; i < thread_count; ++i) {
      threads.emplace_back([&] { io.run(); });
    }
    for (auto& t : threads) t.join();
    watchdog.cancel();

    auto r = rdma_bench::make_base_result(opt, cmd);
    r.scenario_name = "connect_disconnect_soak";
    r.posted_count = opt.iterations * 2;
    r.completed_count = server_completed.load() + client_completed.load();
    r.errors = errors.load();
    r.validation_passed = (r.errors == 0 && r.completed_count == r.posted_count);
    if (!*r.validation_passed) {
      r.first_error = "connect/disconnect soak mismatch or error";
      r.exit_code = 1;
    }
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
