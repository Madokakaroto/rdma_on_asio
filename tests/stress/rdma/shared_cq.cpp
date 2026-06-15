#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/buffer.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> server_pair(rdma::rdma_device_ptr device,
                                  rdma::rdma_listener<tcp>& listener,
                                  rdma_bench::options opt,
                                  std::atomic<std::uint64_t>& completed,
                                  std::atomic<std::uint64_t>& errors) {
  auto [ecg, conn, req_len] =
      co_await listener.async_get_connection(asio::mutable_buffer{}, nothrow);
  (void)req_len;
  if (ecg) {
    ++errors;
    co_return;
  }
  auto ex = co_await asio::this_coro::executor;
  auto& io = static_cast<asio::io_context&>(ex.context());
  rdma::rdma_queue_pair qp(io);
  auto [eca] = co_await conn.async_accept(qp, asio::const_buffer{}, nothrow);
  if (eca) {
    ++errors;
    co_return;
  }

  std::vector<char> storage(opt.message_size, 0);
  rdma::rdma_memory_region mr(device, storage.data(), storage.size());
  for (std::uint64_t i = 0; i < opt.iterations; ++i) {
    auto [er, n] =
        co_await qp.async_recv(mr.slice(std::size_t{0}, storage.size()),
                               nothrow);
    if (er || n != storage.size()) {
      ++errors;
      co_return;
    }
    auto [es, sn] =
        co_await qp.async_send(mr.cslice(std::size_t{0}, n), nothrow);
    if (es || sn != n) {
      ++errors;
      co_return;
    }
    ++completed;
  }
  co_await conn.async_wait_disconnect(nothrow);
}

asio::awaitable<void> client_pair(rdma::rdma_device_ptr device,
                                  rdma_bench::options opt, std::uint16_t port,
                                  std::atomic<std::uint64_t>& completed,
                                  std::atomic<std::uint64_t>& errors) {
  auto ex = co_await asio::this_coro::executor;
  auto& io = static_cast<asio::io_context&>(ex.context());
  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);
  tcp::endpoint ep(asio::ip::make_address(opt.local_addr), port);
  auto [ecc, reply_len] = co_await conn.async_connect(
      qp, ep, asio::const_buffer{}, asio::mutable_buffer{}, nothrow);
  (void)reply_len;
  if (ecc) {
    ++errors;
    co_return;
  }

  std::vector<char> storage(opt.message_size, 0);
  for (std::size_t i = 0; i < storage.size(); ++i) {
    storage[i] = static_cast<char>((i * 13) & 0x7f);
  }
  rdma::rdma_memory_region mr(device, storage.data(), storage.size());
  for (std::uint64_t i = 0; i < opt.iterations; ++i) {
    auto [es, sn] =
        co_await qp.async_send(mr.cslice(std::size_t{0}, storage.size()),
                               nothrow);
    if (es || sn != storage.size()) {
      ++errors;
      co_return;
    }
    auto [er, rn] =
        co_await qp.async_recv(mr.slice(std::size_t{0}, storage.size()),
                               nothrow);
    if (er || rn != storage.size()) {
      ++errors;
      co_return;
    }
    ++completed;
  }
  conn.disconnect();
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto cmd = rdma_bench::command_line(argc, argv);
    if (opt.local_addr.empty()) {
      throw std::invalid_argument("--local-addr is required");
    }
    if (opt.mode != "event") {
      auto r = rdma_bench::make_skip_result(
          opt, cmd, "shared-CQ stress targets event-mode QPs",
          "stress_shared_cq_" + opt.mode);
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }

    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    std::vector<std::unique_ptr<rdma::rdma_listener<tcp>>> listeners;
    listeners.reserve(opt.qps);
    for (std::uint32_t i = 0; i < opt.qps; ++i) {
      auto listener = std::make_unique<rdma::rdma_listener<tcp>>(io);
      listener->open(tcp::v4());
      listener->bind(static_cast<std::uint16_t>(opt.port + i));
      listener->listen();
      listeners.push_back(std::move(listener));
    }

    std::atomic<std::uint64_t> server_completed{0};
    std::atomic<std::uint64_t> client_completed{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<int> remaining{static_cast<int>(opt.qps * 2)};
    auto on_done = [&](std::exception_ptr e) {
      if (e) ++errors;
      if (--remaining == 0) io.stop();
    };

    for (std::uint32_t i = 0; i < opt.qps; ++i) {
      asio::co_spawn(io,
                     server_pair(device, *listeners[i], opt, server_completed,
                                 errors),
                     on_done);
      asio::co_spawn(io,
                     client_pair(device, opt,
                                 static_cast<std::uint16_t>(opt.port + i),
                                 client_completed, errors),
                     on_done);
    }

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
    threads.reserve(thread_count);
    for (std::uint32_t i = 0; i < thread_count; ++i) {
      threads.emplace_back([&] { io.run(); });
    }
    for (auto& t : threads) t.join();
    watchdog.cancel();

    auto r = rdma_bench::make_base_result(opt, cmd);
    r.scenario_name = "shared_cq_stress";
    r.posted_count = opt.qps * opt.iterations * 2;
    r.completed_count = server_completed.load() + client_completed.load();
    r.payload_bytes = r.completed_count * opt.message_size;
    r.errors = errors.load();
    r.validation_passed = (r.errors == 0 && r.completed_count == r.posted_count);
    if (!*r.validation_passed) {
      r.first_error = "shared-CQ stress completion count mismatch or error";
      r.exit_code = 1;
    }
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
