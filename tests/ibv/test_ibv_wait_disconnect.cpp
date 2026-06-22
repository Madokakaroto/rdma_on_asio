// Functional test for connector::async_wait_disconnect (on_disconnect) + the
// synchronous connector::disconnect(). Single process: a server and a client
// coroutine share one io_context and connect over the local RoCE device. The
// client disconnect()s; we assert the server's async_wait_disconnect fires with
// rdma_errc::disconnected, and that arming it again afterwards completes
// immediately (level-triggered).
//
// Usage: test_ibv_wait_disconnect <roce-ip> [port]
// (Needs a working RDMA device + an IP bound to it; skips if no arg given.)
#include <chrono>
#include <iostream>
#include <string>

#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/as_tuple.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

struct result_t {
  bool established = false;
  asio::error_code watcher_ec{};
  bool watcher_fired = false;
  bool level_trigger_ok = false;
};

asio::awaitable<void> run_server(asio::io_context& io,
                                 rdma::rdma_device_ptr const& device,
                                 uint16_t port, result_t& out) {
  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  auto [ec_get, conn, rqn] =
      co_await listener.async_get_connection(asio::mutable_buffer{}, nothrow);
  if (ec_get) {
    std::cerr << "[server] get_connection: " << ec_get.message() << "\n";
    io.stop();
    co_return;
  }
  rdma::rdma_queue_pair qp(io);
  std::string reply = "srv";
  auto [ec_acc] = co_await conn.async_accept(qp, asio::buffer(reply), nothrow);
  if (ec_acc) {
    std::cerr << "[server] accept: " << ec_acc.message() << "\n";
    io.stop();
    co_return;
  }
  out.established = true;
  std::cout << "[server] accepted; arming async_wait_disconnect\n";

  // Block until the peer disconnects (on_disconnect).
  auto [wec] = co_await conn.async_wait_disconnect(nothrow);
  out.watcher_ec = wec;
  out.watcher_fired = true;
  std::cout << "[server] wait_disconnect fired: " << wec.message() << "\n";

  // Level-trigger: connection already torn down -> arming again completes now.
  auto [wec2] = co_await conn.async_wait_disconnect(nothrow);
  out.level_trigger_ok = (wec2 == rdma::rdma_errc::disconnected);
  std::cout << "[server] level-trigger re-arm: " << wec2.message() << "\n";

  io.stop();
}

asio::awaitable<void> run_client(asio::io_context& io,
                                 rdma::rdma_device_ptr const& device,
                                 std::string host, uint16_t port) {
  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);
  tcp::endpoint ep(asio::ip::make_address(host), port);
  std::string req = "cli";
  auto [ec, rpn] = co_await conn.async_connect(qp, ep, asio::buffer(req),
                                               asio::mutable_buffer{}, nothrow);
  if (ec) {
    std::cerr << "[client] connect: " << ec.message() << "\n";
    co_return;
  }
  std::cout << "[client] connected; waiting then disconnecting\n";
  // Give the server a beat to arm its watcher (not required for correctness --
  // a queued DISCONNECTED is read when the watcher arms -- but makes the trace
  // deterministic).
  asio::steady_timer t(io);
  t.expires_after(300ms);
  co_await t.async_wait(nothrow);

  conn.disconnect();  // synchronous, non-blocking teardown
  std::cout << "[client] disconnected (sync)\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "[SKIP] usage: " << argv[0] << " <roce-ip> [port] "
              << "(needs a working RDMA device + IP)\n";
    return 0;
  }
  std::string host = argv[1];
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5007;

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device({});
    rdma::use_device(io, device);

    result_t r;
    asio::co_spawn(io, run_server(io, device, port, r), asio::detached);
    asio::co_spawn(io, run_client(io, device, host, port), asio::detached);
    io.run();

    bool ok = r.established && r.watcher_fired &&
              r.watcher_ec == rdma::rdma_errc::disconnected &&
              r.level_trigger_ok;
    if (ok) {
      std::cout << "[PASS] async_wait_disconnect fired with disconnected; "
                   "level-trigger ok\n";
      return 0;
    }
    std::cerr << "[FAIL] established=" << r.established
              << " watcher_fired=" << r.watcher_fired
              << " ec=" << r.watcher_ec.message()
              << " level_trigger_ok=" << r.level_trigger_ok << "\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
