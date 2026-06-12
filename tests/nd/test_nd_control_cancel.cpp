// NetworkDirect per-operation cancellation coverage for control-plane ops.
//
// Usage: test_nd_control_cancel <nd-ip> [port]
// Skips when no IP is provided because it needs a working ND-capable address.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancellation_signal.hpp"
#include "asio/cancellation_type.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;

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

bool phase_connect_cancel(rdma::rdma_device_ptr const& device,
                          std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  rdma::rdma_connector<tcp> cli(io);
  cli.open(tcp::v4());
  rdma::rdma_queue_pair qp_c(io);

  std::atomic<bool> got_req{false};
  std::atomic<bool> conn_done{false};
  asio::error_code conn_ec;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "connect_cancel", timed_out);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, server_conn, rqn] = co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) {
          co_return;
        }
        got_req.store(true, std::memory_order_release);
        asio::steady_timer park(io);
        park.expires_after(60s);
        co_await park.async_wait(nothrow);
      },
      asio::detached);

  asio::cancellation_signal sig;
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  std::string req = "c";
  cli.async_connect(
      qp_c, ep, asio::buffer(req), asio::mutable_buffer{},
      asio::bind_cancellation_slot(
          sig.slot(), [&](asio::error_code ec, std::size_t) {
            conn_ec = ec;
            conn_done.store(true, std::memory_order_release);
          }));

  std::thread worker([&] { io.run(); });

  bool const reqd =
      wait_until([&] { return got_req.load(std::memory_order_acquire); }, 5s);
  std::this_thread::sleep_for(100ms);
  sig.emit(asio::cancellation_type::terminal);

  bool const fired = wait_until(
      [&] { return conn_done.load(std::memory_order_acquire); }, 5s);

  std::atomic<bool> re_done{false};
  asio::error_code re_ec;
  bool re_fired = false;
  if (fired) {
    cli.async_connect(qp_c, ep, asio::buffer(req), asio::mutable_buffer{},
                      [&](asio::error_code ec, std::size_t) {
      re_ec = ec;
      re_done.store(true, std::memory_order_release);
    });
    re_fired = wait_until(
        [&] { return re_done.load(std::memory_order_acquire); }, 5s);
  }

  io.stop();
  worker.join();

  bool const ok = reqd && fired &&
                  conn_ec == asio::error::operation_aborted &&
                  re_fired && !timed_out &&
                  re_ec == rdma::rdma_errc::connector_terminal;
  if (ok) {
    std::cout << "[PASS] async_connect cancellation slot aborts op; "
                 "connector becomes terminal\n";
  } else {
    std::cerr << "[FAIL] connect cancel: reqd=" << reqd
              << " fired=" << fired << " re_fired=" << re_fired
              << " timed_out=" << timed_out << "\n";
    print_error("[FAIL] connect cancel ec: ", conn_ec);
    print_error("[FAIL] reconnect ec: ", re_ec);
  }
  return ok;
}

bool phase_get_connection_cancel(rdma::rdma_device_ptr const& device,
                                 std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  bool g1_cancelled = false;
  bool g2_ok = false;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "get_connection_cancel", timed_out);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        asio::steady_timer t(io);
        t.expires_after(300ms);
        auto r =
            co_await (lis.async_get_connection(asio::mutable_buffer{}, nothrow) ||
                      t.async_wait(nothrow));
        g1_cancelled = (r.index() == 1);

        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
              rdma::rdma_connector<tcp> cli(io);
              cli.open(tcp::v4());
              rdma::rdma_queue_pair qp(io);
              tcp::endpoint ep(asio::ip::make_address(ip), port);
              std::string req = "c";
              co_await cli.async_connect(qp, ep, asio::buffer(req), asio::mutable_buffer{}, nothrow);
            },
            asio::detached);

        auto [e2, c2, rqn2] = co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
        (void)c2;
        g2_ok = !e2;
        io.stop();
      },
      asio::detached);

  io.run();

  bool const ok = g1_cancelled && g2_ok && !timed_out;
  if (ok) {
    std::cout << "[PASS] async_get_connection cancellation leaves listener "
                 "reusable\n";
  } else {
    std::cerr << "[FAIL] get_connection cancel: g1_cancelled="
              << g1_cancelled << " g2_ok=" << g2_ok
              << " timed_out=" << timed_out << "\n";
  }
  return ok;
}

bool phase_wait_disconnect_cancel(rdma::rdma_device_ptr const& device,
                                  std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  bool established = false;
  bool wait_cancelled = false;
  bool alive_after = false;
  bool timed_out = false;
  asio::steady_timer watchdog(io);
  arm_watchdog(io, watchdog, "wait_disconnect_cancel", timed_out);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, conn, rqn] = co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) {
          print_error("[server] get_connection: ", ecg);
          io.stop();
          co_return;
        }
        rdma::rdma_queue_pair qp_s(io);
        std::string rep = "s";
        auto [eca] =
            co_await conn.async_accept(qp_s, asio::buffer(rep), nothrow);
        if (eca) {
          print_error("[server] accept: ", eca);
          io.stop();
          co_return;
        }
        std::array<char, 64> buf{};
        rdma::rdma_memory_region mr(device, buf.data(), buf.size());
        co_await qp_s.async_recv(mr.slice(std::size_t{0}, buf.size()),
                                 nothrow);
        co_await conn.async_wait_disconnect(nothrow);
        io.stop();
      },
      asio::detached);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp_c(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "c";
        auto [ecc, rpn] = co_await conn.async_connect(
            qp_c, ep, asio::buffer(req), asio::mutable_buffer{}, nothrow);
        if (ecc) {
          print_error("[client] connect: ", ecc);
          co_return;
        }
        established = true;

        asio::steady_timer t(io);
        t.expires_after(300ms);
        auto r =
            co_await (conn.async_wait_disconnect(nothrow) ||
                      t.async_wait(nothrow));
        wait_cancelled = (r.index() == 1);

        std::array<char, 64> sbuf{};
        std::memcpy(sbuf.data(), "alive", 5);
        rdma::rdma_memory_region smr(device, sbuf.data(), sbuf.size());
        auto [sec, sn] =
            co_await qp_c.async_send(smr.cslice(std::size_t{0}, 5), nothrow);
        (void)sn;
        if (sec) {
          print_error("[client] send after wait cancel: ", sec);
        }
        alive_after = !sec;
        conn.disconnect();
      },
      asio::detached);

  io.run();

  bool const ok = established && wait_cancelled && alive_after && !timed_out;
  if (ok) {
    std::cout << "[PASS] async_wait_disconnect cancellation does not tear down "
                 "the connection\n";
  } else {
    std::cerr << "[FAIL] wait_disconnect cancel: established=" << established
              << " wait_cancelled=" << wait_cancelled
              << " alive_after=" << alive_after
              << " timed_out=" << timed_out << "\n";
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
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5040;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});

    bool ok = true;
    ok &= phase_connect_cancel(device, ip, port);
    ok &= phase_get_connection_cancel(device, ip,
                                      static_cast<uint16_t>(port + 1));
    ok &= phase_wait_disconnect_cancel(device, ip,
                                       static_cast<uint16_t>(port + 2));

    if (ok) {
      std::cout << "\nAll nd control-plane cancellation tests passed.\n";
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
