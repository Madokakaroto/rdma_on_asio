// NetworkDirect coverage for state-adaptive connector::disconnect().
//
// Usage: test_nd_disconnect_cancel <nd-ip> [port]
// Skips when no IP is provided because it needs a working ND-capable address.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

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

void arm_watchdog(asio::io_context& io, asio::steady_timer& timer,
                  char const* phase, bool& timed_out) {
  timer.expires_after(12s);
  timer.async_wait([&io, &timed_out, phase](asio::error_code ec) {
    if (!ec) {
      timed_out = true;
      std::cerr << "[TIMEOUT] " << phase << "\n";
      io.stop();
    }
  });
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
  auto const deadline = std::chrono::steady_clock::now() + budget;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

bool phase_pending_connect_abort(rdma::rdma_device_ptr const& device,
                                 std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);

  std::atomic<bool> got_req{false};
  std::atomic<bool> done{false};
  asio::error_code connect_ec;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "pending_connect_abort", timed_out);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, server_conn, rqn] =
            co_await listener.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) {
          co_return;
        }
        got_req.store(true, std::memory_order_release);
        asio::steady_timer park(io);
        park.expires_after(60s);
        co_await park.async_wait(nothrow);
      },
      asio::detached);

  std::string req = "x";
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  conn.async_connect(qp, ep, asio::buffer(req), asio::mutable_buffer{},
                     [&](asio::error_code ec, std::size_t) {
    connect_ec = ec;
    done.store(true, std::memory_order_release);
  });

  std::thread worker([&] { io.run(); });

  bool const reqd = wait_until(
      [&] { return got_req.load(std::memory_order_acquire); }, 5s);
  std::this_thread::sleep_for(100ms);
  conn.disconnect();

  bool const fired =
      wait_until([&] { return done.load(std::memory_order_acquire); }, 5s);
  io.stop();
  worker.join();

  bool const ok = reqd && fired &&
                  connect_ec == asio::error::operation_aborted &&
                  !timed_out;
  if (ok) {
    std::cout << "[PASS] pending async_connect aborted by cross-thread "
                 "disconnect()\n";
  } else {
    std::cerr << "[FAIL] pending connect abort: got_req=" << reqd
              << " fired=" << fired << " timed_out=" << timed_out << "\n";
    print_error("[FAIL] pending connect ec: ", connect_ec);
  }
  return ok;
}

bool phase_established_recv_abort(rdma::rdma_device_ptr const& device,
                                  std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  rdma::rdma_connector<tcp> client(io);
  client.open(tcp::v4());
  rdma::rdma_queue_pair client_qp(io);

  std::atomic<bool> recv_armed{false};
  std::atomic<bool> recv_done{false};
  asio::error_code recv_ec;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "established_recv_abort", timed_out);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, server_conn, rqn] =
            co_await listener.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) {
          print_error("[server] get_connection: ", ecg);
          co_return;
        }
        rdma::rdma_queue_pair server_qp(io);
        std::string reply = "s";
        auto [eca] =
            co_await server_conn.async_accept(server_qp, asio::buffer(reply),
                                              nothrow);
        if (eca) {
          print_error("[server] accept: ", eca);
          co_return;
        }
        co_await server_conn.async_wait_disconnect(nothrow);
      },
      asio::detached);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "c";
        auto [ecc, rpn] = co_await client.async_connect(
            client_qp, ep, asio::buffer(req), asio::mutable_buffer{}, nothrow);
        if (ecc) {
          print_error("[client] connect: ", ecc);
          co_return;
        }

        std::array<char, 4096> buf{};
        rdma::rdma_memory_region mr(device, buf.data(), buf.size());
        recv_armed.store(true, std::memory_order_release);
        auto [er, n] =
            co_await client_qp.async_recv(mr.slice(std::size_t{0}, buf.size()),
                                          nothrow);
        (void)n;
        recv_ec = er;
        recv_done.store(true, std::memory_order_release);
      },
      asio::detached);

  std::thread worker([&] { io.run(); });

  bool const armed = wait_until(
      [&] { return recv_armed.load(std::memory_order_acquire); }, 5s);
  std::this_thread::sleep_for(200ms);
  client.disconnect();

  bool const fired = wait_until(
      [&] { return recv_done.load(std::memory_order_acquire); }, 5s);
  io.stop();
  worker.join();

  bool const ok = armed && fired &&
                  recv_ec == asio::error::operation_aborted &&
                  !timed_out;
  if (ok) {
    std::cout << "[PASS] established pending recv aborted by cross-thread "
                 "disconnect()\n";
  } else {
    std::cerr << "[FAIL] established recv abort: armed=" << armed
              << " fired=" << fired << " timed_out=" << timed_out << "\n";
    print_error("[FAIL] recv ec: ", recv_ec);
  }
  return ok;
}

bool phase_terminal_reconnect(rdma::rdma_device_ptr const& device,
                              std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  conn.disconnect();

  rdma::rdma_queue_pair qp(io);
  std::atomic<bool> done{false};
  asio::error_code connect_ec;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "terminal_reconnect", timed_out);
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  std::string req = "x";
  conn.async_connect(qp, ep, asio::buffer(req), asio::mutable_buffer{},
                     [&](asio::error_code ec, std::size_t) {
    connect_ec = ec;
    done.store(true, std::memory_order_release);
  });

  std::thread worker([&] { io.run(); });
  bool const fired =
      wait_until([&] { return done.load(std::memory_order_acquire); }, 5s);
  io.stop();
  worker.join();

  bool const ok = fired && !timed_out &&
                  connect_ec == rdma::nd_errc::ext_connector_terminal;
  if (ok) {
    std::cout << "[PASS] reconnect on terminal connector returns "
                 "ext_connector_terminal\n";
  } else {
    std::cerr << "[FAIL] terminal reconnect: fired=" << fired
              << " timed_out=" << timed_out << "\n";
    print_error("[FAIL] terminal reconnect ec: ", connect_ec);
  }
  return ok;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "[SKIP] usage: " << argv[0] << " <nd-ip> [port] "
              << "(needs a working NetworkDirect device + IP)\n";
    return 0;
  }

  std::string ip = argv[1];
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5012;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});

    bool ok = true;
    ok &= phase_pending_connect_abort(device, ip,
                                      static_cast<uint16_t>(port + 113));
    ok &= phase_established_recv_abort(device, ip, port);
    ok &= phase_terminal_reconnect(device, ip, static_cast<uint16_t>(port + 1));

    if (ok) {
      std::cout << "\nAll nd disconnect/cancel tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more phases failed.\n";
    return 1;
  }
  catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
