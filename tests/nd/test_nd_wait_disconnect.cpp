// Functional test for connector::async_wait_disconnect on NetworkDirect.
//
// Usage: test_nd_wait_disconnect <nd-ip> [port]
// Skips when no IP is provided because it needs a working ND-capable address.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

void print_error(char const* prefix, asio::error_code const& ec) {
  std::cerr << prefix << ec.message() << " (value=0x" << std::hex
            << static_cast<unsigned>(ec.value()) << std::dec
            << ", category=" << ec.category().name() << ")\n";
}

struct result_t {
  bool established = false;
  asio::error_code watcher_ec{};
  asio::error_code client_ec{};
  asio::error_code server_ec{};
  bool watcher_fired = false;
  bool level_trigger_ok = false;
  bool client_done = false;
  bool timed_out = false;
  std::string phase = "starting";
};

asio::awaitable<void> run_server(asio::io_context& io,
                                 rdma::rdma_device_ptr const& device,
                                 uint16_t port, result_t& out) {
  rdma::rdma_listener<tcp> listener(io);
  out.phase = "server.open";
  listener.open(tcp::v4());
  out.phase = "server.bind";
  listener.bind(port);
  out.phase = "server.listen";
  listener.listen();

  out.phase = "server.get_connection";
  auto [ec_get, conn, rqn] = co_await listener.async_get_connection(asio::mutable_buffer{}, nothrow);
  if (ec_get) {
    out.server_ec = ec_get;
    print_error("[server] get_connection: ", ec_get);
    io.stop();
    co_return;
  }

  rdma::rdma_queue_pair qp(io);
  std::string reply = "srv";
  out.phase = "server.accept";
  auto [ec_acc] = co_await conn.async_accept(qp, asio::buffer(reply), nothrow);
  if (ec_acc) {
    out.server_ec = ec_acc;
    print_error("[server] accept: ", ec_acc);
    io.stop();
    co_return;
  }

  out.established = true;
  out.phase = "server.wait_disconnect";
  auto [wec] = co_await conn.async_wait_disconnect(nothrow);
  out.watcher_ec = wec;
  out.watcher_fired = true;

  out.phase = "server.wait_disconnect_level_trigger";
  auto [wec2] = co_await conn.async_wait_disconnect(nothrow);
  out.level_trigger_ok = (wec2 == rdma::rdma_errc::disconnected);
  out.phase = "done";
  io.stop();
}

asio::awaitable<void> run_client(asio::io_context& io,
                                 std::string host, uint16_t port,
                                 result_t& out) {
  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);

  tcp::endpoint ep(asio::ip::make_address(host), port);
  std::string req = "cli";
  out.phase = "client.connect";
  auto [ec, rpn] = co_await conn.async_connect(qp, ep, asio::buffer(req), asio::mutable_buffer{}, nothrow);
  if (ec) {
    out.client_ec = ec;
    out.client_done = true;
    print_error("[client] connect: ", ec);
    io.stop();
    co_return;
  }

  asio::steady_timer t(io);
  t.expires_after(300ms);
  out.phase = "client.connected_pause";
  co_await t.async_wait(nothrow);

  out.phase = "client.disconnect";
  conn.disconnect();
  out.client_done = true;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "[SKIP] usage: " << argv[0] << " <nd-ip> [port] "
              << "(needs a working NetworkDirect device + IP)\n";
    return 0;
  }

  std::string host = argv[1];
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5007;

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    result_t r;
    asio::steady_timer watchdog(io);
    watchdog.expires_after(10s);
    watchdog.async_wait([&](asio::error_code ec) {
      if (!ec) {
        r.timed_out = true;
        std::cerr << "[TIMEOUT] phase=" << r.phase << "\n";
        io.stop();
      }
    });

    asio::co_spawn(io, run_server(io, device, port, r), asio::detached);
    asio::co_spawn(io, run_client(io, host, port, r), asio::detached);
    io.run();

    bool const ok = r.established && r.watcher_fired &&
                    r.watcher_ec == rdma::rdma_errc::disconnected &&
                    r.level_trigger_ok && !r.timed_out;
    if (ok) {
      std::cout << "[PASS] async_wait_disconnect fired with disconnected; "
                   "level-trigger ok\n";
      return 0;
    }

    std::cerr << "[FAIL] established=" << r.established
              << " client_done=" << r.client_done
              << " watcher_fired=" << r.watcher_fired
              << " ec=" << r.watcher_ec.message()
              << " level_trigger_ok=" << r.level_trigger_ok
              << " timed_out=" << r.timed_out
              << " phase=" << r.phase << "\n";
    if (r.client_ec) {
      print_error("[FAIL] client_ec: ", r.client_ec);
    }
    if (r.server_ec) {
      print_error("[FAIL] server_ec: ", r.server_ec);
    }
    return 1;
  }
  catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
